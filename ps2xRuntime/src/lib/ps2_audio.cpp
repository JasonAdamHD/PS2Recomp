#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}

namespace
{
    // Ring buffer between the IOP thread (producer) and the host audio callback (consumer).
    struct SpuStream
    {
        static constexpr size_t kCapacityFrames = 48000u; // 1 s
        static constexpr size_t kTargetFrames = 4800u;    // 100 ms of latency
        static constexpr size_t kMaxFrames = 14400u;      // beyond 300 ms, drop the backlog to the target

        std::mutex mutex;
        std::vector<int16_t> ring = std::vector<int16_t>(kCapacityFrames * 2u);
        size_t readFrame = 0;
        size_t frames = 0;
        bool primed = false;
        bool started = false;

        void push(const int16_t *data, size_t count)
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < count; ++i)
            {
                if (frames == kCapacityFrames)
                {
                    readFrame = (readFrame + 1u) % kCapacityFrames;
                    --frames;
                }
                const size_t write = (readFrame + frames) % kCapacityFrames;
                ring[write * 2u] = data[i * 2u];
                ring[write * 2u + 1u] = data[i * 2u + 1u];
                ++frames;
            }
            if (frames > kMaxFrames)
            {
                const size_t drop = frames - kTargetFrames;
                readFrame = (readFrame + drop) % kCapacityFrames;
                frames -= drop;
            }
            if (frames >= kTargetFrames)
                primed = true;
        }

        void pull(int16_t *out, size_t count)
        {
            std::lock_guard<std::mutex> lock(mutex);
            size_t i = 0;
            if (primed)
            {
                for (; i < count && frames > 0u; ++i)
                {
                    out[i * 2u] = ring[readFrame * 2u];
                    out[i * 2u + 1u] = ring[readFrame * 2u + 1u];
                    readFrame = (readFrame + 1u) % kCapacityFrames;
                    --frames;
                }
                if (frames == 0u)
                    primed = false; // underrun: wait for the buffer to refill before playing again
            }
            std::fill(out + i * 2u, out + count * 2u, int16_t{0});
        }
    };

    SpuStream &spuStream()
    {
        static SpuStream stream;
        return stream;
    }

#if !defined(PLATFORM_VITA)
    void spuStreamCallback(void *buffer, unsigned int frames)
    {
        spuStream().pull(static_cast<int16_t *>(buffer), frames);
    }
#endif
}

void PS2AudioBackend::pushSpuOutput(const int16_t *interleaved, size_t frames)
{
#if defined(PLATFORM_VITA)
    (void)interleaved;
    (void)frames;
#else
    if (!m_audioReady || !interleaved || frames == 0u)
        return;
    SpuStream &stream = spuStream();
    if (!stream.started)
    {
        stream.started = true;
        SetAudioStreamBufferSizeDefault(1024);
        AudioStream audio = LoadAudioStream(48000u, 16u, 2u);
        SetAudioStreamCallback(audio, &spuStreamCallback);
        PlayAudioStream(audio);
    }
    stream.push(interleaved, frames);

    // PS2X_SPU_WAV=<path> records the first minute of SPU2 output as a 48 kHz stereo WAV.
    static FILE *wav = nullptr;
    static uint32_t wavFrames = 0;
    static bool wavChecked = false;
    if (!wavChecked)
    {
        wavChecked = true;
        if (const char *path = std::getenv("PS2X_SPU_WAV"))
            wav = std::fopen(path, "wb");
        if (wav)
        {
            const std::vector<uint8_t> header = buildWavFromPcm(nullptr, 0u, 48000u);
            std::fwrite(header.data(), 1u, header.size(), wav);
        }
    }
    if (wav && wavFrames < 48000u * 60u)
    {
        std::fwrite(interleaved, sizeof(int16_t) * 2u, frames, wav);
        wavFrames += static_cast<uint32_t>(frames);
        if (wavFrames >= 48000u * 60u)
        {
            // Patch the header for stereo and the final data size.
            const uint32_t dataBytes = wavFrames * 4u;
            const uint32_t riffBytes = 36u + dataBytes;
            const uint16_t channels = 2u, blockAlign = 4u;
            const uint32_t byteRate = 48000u * 4u;
            std::fseek(wav, 4, SEEK_SET);
            std::fwrite(&riffBytes, 4u, 1u, wav);
            std::fseek(wav, 22, SEEK_SET);
            std::fwrite(&channels, 2u, 1u, wav);
            std::fseek(wav, 28, SEEK_SET);
            std::fwrite(&byteRate, 4u, 1u, wav);
            std::fwrite(&blockAlign, 2u, 1u, wav);
            std::fseek(wav, 40, SEEK_SET);
            std::fwrite(&dataBytes, 4u, 1u, wav);
            std::fclose(wav);
            wav = nullptr;
        }
    }
#endif
}
