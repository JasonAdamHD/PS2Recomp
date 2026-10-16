// Netplay: frame-locked inputs, input record/replay and UDP lockstep. See runtime/ps2_netplay.h.
// Deliberately free of runtime/raylib headers: the socket API pulls in <windows.h>.
#include "runtime/ps2_netplay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace ps2netplay
{
    namespace
    {
        constexpr uint32_t kMagic = 0x32584E50u; // "PNX2"
        constexpr uint8_t kVersion = 1;
        constexpr int kMaxPlayers = 4;
        constexpr int kRedundancy = 16;          // each INPUT packet repeats the sender's last 16 frames
        constexpr uint64_t kHashInterval = 60u;  // netplay desync check period (ticks)
        constexpr uint16_t kDefaultPort = 7777;

        enum class Role
        {
            Off,
            Record,
            Replay,
            Host,
            Client
        };

        enum PacketType : uint8_t
        {
            Hello = 1,   // client -> host: {version}
            Welcome = 2, // host -> client: {version, player, players, delay, epoch}
            Start = 3,   // host -> clients: everyone has joined
            Input = 4,   // {player, firstTick, count, inputs...}; the host relays clients' packets to the others
            Hash = 5,    // {player, tick, hash}; relayed like Input
            Bye = 6,     // {player}
            CardRequest = 7, // client -> host: {offset} into the memory card image
            CardChunk = 8,   // host -> client: {total, offset, length, bytes}
        };
        constexpr uint32_t kCardChunk = 1024u;

        struct Frame
        {
            std::array<PadInput, kMaxPlayers> input{};
            uint8_t have = 0;
        };

        struct Writer
        {
            std::vector<uint8_t> bytes;
            explicit Writer(PacketType type)
            {
                u32(kMagic);
                u8(type);
            }
            void u8(uint8_t v) { bytes.push_back(v); }
            void u16(uint16_t v) { put(&v, 2); }
            void u32(uint32_t v) { put(&v, 4); }
            void u64(uint64_t v) { put(&v, 8); }
            void put(const void *data, size_t size)
            {
                const auto *p = static_cast<const uint8_t *>(data);
                bytes.insert(bytes.end(), p, p + size);
            }
            void input(const PadInput &in)
            {
                u16(in.buttons);
                u8(in.rx);
                u8(in.ry);
                u8(in.lx);
                u8(in.ly);
            }
        };

        struct Reader
        {
            const uint8_t *p;
            size_t left;
            bool ok = true;
            bool take(void *out, size_t size)
            {
                if (left < size)
                    return ok = false;
                std::memcpy(out, p, size);
                p += size;
                left -= size;
                return true;
            }
            uint8_t u8()
            {
                uint8_t v = 0;
                take(&v, 1);
                return v;
            }
            uint16_t u16()
            {
                uint16_t v = 0;
                take(&v, 2);
                return v;
            }
            uint32_t u32()
            {
                uint32_t v = 0;
                take(&v, 4);
                return v;
            }
            uint64_t u64()
            {
                uint64_t v = 0;
                take(&v, 8);
                return v;
            }
            PadInput input()
            {
                PadInput in;
                in.buttons = u16();
                in.rx = u8();
                in.ry = u8();
                in.lx = u8();
                in.ly = u8();
                return in;
            }
        };

        struct State
        {
            Role role = Role::Off;
            bool initialized = false;
            bool failed = false;
            int players = 1;
            int localPlayer = 0;
            int delay = 3;
            uint64_t hashEvery = 0;
            std::time_t epoch = 0;
            uint64_t currentTick = 0;
            std::array<PadInput, kMaxPlayers> latched{};
            uint8_t presentMask = 1;    // ports owned by a player
            uint8_t droppedMask = 0;    // players that timed out: idle from then on
            std::map<uint64_t, Frame> frames;
            std::map<uint64_t, uint64_t> localHashes;
            std::map<uint64_t, std::map<int, uint64_t>> remoteHashes; // arrived before ours was computed
            std::vector<PadInput> localHistory; // local input by tick (index = tick), for redundant sends
            bool (*sampler)(PS2Runtime &, PadInput &) = nullptr;

            // Sockets (Host / Client).
            SocketHandle socket = kNoSocket;
            sockaddr_in hostAddress{};
            std::array<sockaddr_in, kMaxPlayers> peer{}; // host: each client's address
            std::array<bool, kMaxPlayers> joined{};
            bool started = false;

            // Memory card (netplay): the host's card image, packed.
            std::filesystem::path (*getCard)() = nullptr;
            void (*setCard)(const std::filesystem::path &) = nullptr;
            std::vector<uint8_t> card;
            uint32_t cardTotal = 0;
            bool cardKnown = false;
            bool cardDone = false;

            // Record / replay.
            FILE *file = nullptr;
            bool replayEnded = false;
        };

        State &state()
        {
            static State s;
            return s;
        }

        void log(const std::string &text)
        {
            std::fprintf(stderr, "[netplay] %s\n", text.c_str());
            std::fflush(stderr);
        }

        int envInt(const char *name, int fallback)
        {
            const char *value = std::getenv(name);
            return value && *value ? std::atoi(value) : fallback;
        }

        // ---- sockets ----

        bool sameAddress(const sockaddr_in &a, const sockaddr_in &b)
        {
            return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
        }

        bool openSocket(uint16_t bindPort)
        {
            State &s = state();
#ifdef _WIN32
            static bool wsa = []
            {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            if (!wsa)
                return false;
#endif
            s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s.socket == kNoSocket)
                return false;
#ifdef _WIN32
            u_long nonBlocking = 1;
            ioctlsocket(s.socket, FIONBIO, &nonBlocking);
#else
            fcntl(s.socket, F_SETFL, fcntl(s.socket, F_GETFL, 0) | O_NONBLOCK);
#endif
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(bindPort);
            return ::bind(s.socket, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) == 0;
        }

        void sendTo(const sockaddr_in &to, const Writer &packet)
        {
            ::sendto(state().socket, reinterpret_cast<const char *>(packet.bytes.data()), int(packet.bytes.size()), 0,
                     reinterpret_cast<const sockaddr *>(&to), sizeof(to));
        }

        void sendRaw(const sockaddr_in &to, const uint8_t *data, size_t size)
        {
            ::sendto(state().socket, reinterpret_cast<const char *>(data), int(size), 0,
                     reinterpret_cast<const sockaddr *>(&to), sizeof(to));
        }

        // Host: forward a client's packet to every other client.
        void relay(int fromPlayer, const uint8_t *data, size_t size)
        {
            State &s = state();
            for (int p = 1; p < s.players; ++p)
                if (p != fromPlayer && s.joined[p])
                    sendRaw(s.peer[p], data, size);
        }

        void broadcast(const Writer &packet)
        {
            State &s = state();
            if (s.role == Role::Client)
            {
                sendTo(s.hostAddress, packet);
                return;
            }
            for (int p = 1; p < s.players; ++p)
                if (s.joined[p])
                    sendTo(s.peer[p], packet);
        }

        void storeInputs(int player, uint64_t firstTick, const std::vector<PadInput> &inputs)
        {
            State &s = state();
            if (player < 0 || player >= s.players || player == s.localPlayer)
                return;
            for (size_t i = 0; i < inputs.size(); ++i)
            {
                const uint64_t tick = firstTick + i;
                if (tick < s.currentTick)
                    continue; // already consumed
                Frame &frame = s.frames[tick];
                frame.input[player] = inputs[i];
                frame.have |= uint8_t(1u << player);
            }
        }

        void reportDesync(int player, uint64_t tick, uint64_t theirs, uint64_t ours)
        {
            if (theirs == ours)
                return;
            char text[160];
            std::snprintf(text, sizeof(text), "DESYNC: player %d's game differs at tick %llu (%016llx vs ours %016llx)",
                          player + 1, static_cast<unsigned long long>(tick), static_cast<unsigned long long>(theirs),
                          static_cast<unsigned long long>(ours));
            log(text);
        }

        void checkHash(int player, uint64_t tick, uint64_t hash)
        {
            State &s = state();
            if (const auto mine = s.localHashes.find(tick); mine != s.localHashes.end())
                reportDesync(player, tick, hash, mine->second);
            else
                s.remoteHashes[tick][player] = hash;
        }

        // Memory card image: {u32 count, then per file {u16 path length, path, u32 size, bytes}}.
        std::vector<uint8_t> packCard(const std::filesystem::path &root)
        {
            Writer w(Hello); // the little-endian writer; its packet header is dropped below
            w.bytes.clear();
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            if (std::filesystem::is_directory(root, ec))
                for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec))
                    if (entry.is_regular_file(ec))
                        files.push_back(entry.path());
            w.u32(uint32_t(files.size()));
            for (const auto &file : files)
            {
                const std::string relative = std::filesystem::relative(file, root, ec).generic_string();
                std::ifstream in(file, std::ios::binary);
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                w.u16(uint16_t(relative.size()));
                w.put(relative.data(), relative.size());
                w.u32(uint32_t(bytes.size()));
                w.put(bytes.data(), bytes.size());
            }
            return w.bytes;
        }

        bool unpackCard(const std::vector<uint8_t> &image, const std::filesystem::path &root)
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root, ec);
            Reader r{image.data(), image.size()};
            const uint32_t count = image.empty() ? 0u : r.u32();
            for (uint32_t i = 0; i < count && r.ok; ++i)
            {
                std::string relative(r.u16(), ' ');
                r.take(relative.data(), relative.size());
                std::vector<uint8_t> bytes(r.u32());
                r.take(bytes.data(), bytes.size());
                if (!r.ok || relative.find("..") != std::string::npos)
                    return false;
                const std::filesystem::path file = root / std::filesystem::path(relative);
                std::filesystem::create_directories(file.parent_path(), ec);
                std::ofstream out(file, std::ios::binary);
                out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
            }
            return r.ok;
        }

        void useSessionCard()
        {
            State &s = state();
            std::error_code ec;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const std::filesystem::path folder =
                std::filesystem::temp_directory_path(ec) / "ps2x_netplay" / ("card_" + std::to_string(stamp));
            if (s.setCard && unpackCard(s.card, folder))
            {
                s.setCard(folder);
                log("playing on the host's memory card (a temporary copy; real saves are not touched)");
            }
            else
            {
                log("could not set up the session memory card");
            }
        }

        void handlePacket(const uint8_t *data, size_t size, const sockaddr_in &from)
        {
            State &s = state();
            Reader r{data, size};
            if (r.u32() != kMagic)
                return;
            const uint8_t type = r.u8();
            switch (type)
            {
            case Hello:
            {
                if (s.role != Role::Host || r.u8() != kVersion)
                    return;
                int player = -1;
                for (int p = 1; p < s.players; ++p)
                    if (s.joined[p] && sameAddress(s.peer[p], from))
                        player = p;
                if (player < 0 && !s.started)
                {
                    for (int p = 1; p < s.players && player < 0; ++p)
                        if (!s.joined[p])
                            player = p;
                    if (player > 0)
                    {
                        s.peer[player] = from;
                        s.joined[player] = true;
                        char text[96];
                        std::snprintf(text, sizeof(text), "player %d joined from %s:%u", player + 1,
                                      inet_ntoa(from.sin_addr), unsigned(ntohs(from.sin_port)));
                        log(text);
                    }
                }
                if (player < 0)
                    return; // session full or already running
                Writer w(Welcome);
                w.u8(kVersion);
                w.u8(uint8_t(player));
                w.u8(uint8_t(s.players));
                w.u8(uint8_t(s.delay));
                w.u64(uint64_t(s.epoch));
                w.u32(uint32_t(s.card.size()));
                sendTo(from, w);
                if (s.started)
                    sendTo(from, Writer(Start));
                return;
            }
            case Welcome:
            {
                if (s.role != Role::Client || s.players > 1)
                    return;
                if (r.u8() != kVersion)
                {
                    log("host runs a different netplay version");
                    return;
                }
                s.localPlayer = r.u8();
                s.players = std::clamp<int>(r.u8(), 2, kMaxPlayers);
                s.delay = r.u8();
                s.epoch = std::time_t(r.u64());
                s.cardTotal = r.u32();
                if (!r.ok)
                    return;
                s.cardKnown = true;
                s.presentMask = uint8_t((1u << s.players) - 1u);
                char text[96];
                std::snprintf(text, sizeof(text), "joined as player %d of %d (input delay %d)", s.localPlayer + 1, s.players, s.delay);
                log(text);
                return;
            }
            case Start:
                if (s.role == Role::Client && s.players > 1)
                    s.started = true;
                return;
            case Input:
            {
                const int player = r.u8();
                const uint64_t firstTick = r.u64();
                const int count = r.u8();
                std::vector<PadInput> inputs;
                for (int i = 0; i < count && r.ok; ++i)
                    inputs.push_back(r.input());
                if (!r.ok)
                    return;
                storeInputs(player, firstTick, inputs);
                if (s.role == Role::Host)
                    relay(player, data, size);
                return;
            }
            case Hash:
            {
                const int player = r.u8();
                const uint64_t tick = r.u64();
                const uint64_t hash = r.u64();
                if (!r.ok)
                    return;
                checkHash(player, tick, hash);
                if (s.role == Role::Host)
                    relay(player, data, size);
                return;
            }
            case CardRequest:
            {
                const uint32_t offset = r.u32();
                if (s.role != Role::Host || !r.ok || offset > s.card.size())
                    return;
                const uint32_t length = std::min<uint32_t>(kCardChunk, uint32_t(s.card.size()) - offset);
                Writer w(CardChunk);
                w.u32(uint32_t(s.card.size()));
                w.u32(offset);
                w.u16(uint16_t(length));
                w.put(s.card.data() + offset, length);
                sendTo(from, w);
                return;
            }
            case CardChunk:
            {
                const uint32_t total = r.u32();
                const uint32_t offset = r.u32();
                const uint16_t length = r.u16();
                if (s.role != Role::Client || !r.ok || s.cardDone || offset != s.card.size() || r.left < length)
                    return;
                s.card.insert(s.card.end(), r.p, r.p + length);
                if (s.card.size() >= total)
                    s.cardDone = true;
                return;
            }
            case Bye:
            {
                const int player = r.u8();
                if (player >= 0 && player < kMaxPlayers && player != s.localPlayer && !(s.droppedMask & (1u << player)))
                {
                    s.droppedMask |= uint8_t(1u << player);
                    log("player " + std::to_string(player + 1) + " left; their pad is idle from now on");
                }
                if (s.role == Role::Host)
                    relay(player, data, size);
                return;
            }
            default:
                return;
            }
        }

        void pump()
        {
            State &s = state();
            uint8_t buffer[1500];
            for (;;)
            {
                sockaddr_in from{};
#ifdef _WIN32
                int fromLength = sizeof(from);
#else
                socklen_t fromLength = sizeof(from);
#endif
                const auto received = ::recvfrom(s.socket, reinterpret_cast<char *>(buffer), int(sizeof(buffer)), 0,
                                                 reinterpret_cast<sockaddr *>(&from), &fromLength);
                if (received <= 0)
                    break;
                handlePacket(buffer, size_t(received), from);
            }
        }

        void sendLocalInputs(uint64_t lastTick)
        {
            State &s = state();
            const uint64_t first = lastTick >= uint64_t(kRedundancy - 1) ? lastTick - (kRedundancy - 1) : 0u;
            Writer w(Input);
            w.u8(uint8_t(s.localPlayer));
            w.u64(first);
            w.u8(uint8_t(lastTick - first + 1));
            for (uint64_t t = first; t <= lastTick; ++t)
                w.input(t < s.localHistory.size() ? s.localHistory[t] : PadInput{});
            broadcast(w);
        }

        // ---- session setup ----

        bool resolve(const std::string &host, uint16_t port, sockaddr_in &out)
        {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo *result = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
                return false;
            out = *reinterpret_cast<const sockaddr_in *>(result->ai_addr);
            out.sin_port = htons(port);
            freeaddrinfo(result);
            return true;
        }

        void connectSession()
        {
            State &s = state();
            using clock = std::chrono::steady_clock;
            if (s.role == Role::Host)
            {
                if (s.getCard)
                    s.card = packCard(s.getCard());
                useSessionCard();
                char text[128];
                std::snprintf(text, sizeof(text), "hosting a %d-player session; waiting for players (UDP port %u)",
                              s.players, unsigned(ntohs(s.hostAddress.sin_port)));
                log(text);
                for (;;)
                {
                    pump();
                    bool all = true;
                    for (int p = 1; p < s.players; ++p)
                        all = all && s.joined[p];
                    if (all)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                s.started = true;
                for (int i = 0; i < 5; ++i)
                    broadcast(Writer(Start));
                log("all players joined; starting");
                return;
            }

            log("connecting to the host...");
            auto nextHello = clock::now();
            auto nextRequest = clock::now();
            size_t requestedAt = SIZE_MAX;
            while (!s.started || !s.cardDone)
            {
                if (!s.cardKnown && clock::now() >= nextHello)
                {
                    Writer w(Hello);
                    w.u8(kVersion);
                    sendTo(s.hostAddress, w);
                    nextHello = clock::now() + std::chrono::milliseconds(250);
                }
                if (s.cardKnown && !s.cardDone)
                {
                    if (s.cardTotal == 0u)
                        s.cardDone = true;
                    else if (s.card.size() != requestedAt || clock::now() >= nextRequest)
                    {
                        requestedAt = s.card.size();
                        Writer w(CardRequest);
                        w.u32(uint32_t(s.card.size()));
                        sendTo(s.hostAddress, w);
                        nextRequest = clock::now() + std::chrono::milliseconds(100);
                    }
                }
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            useSessionCard();
            log("session started");
        }

        void initialize()
        {
            State &s = state();
            if (s.initialized)
                return;
            s.initialized = true;
            s.epoch = std::time(nullptr);
            s.hashEvery = uint64_t(std::max(0, envInt("PS2X_FRAMEHASH", 0)));

            if (const char *net = std::getenv("PS2X_NETPLAY"); net && *net)
            {
                std::string spec(net);
                uint16_t port = kDefaultPort;
                std::string address;
                if (spec.rfind("host", 0) == 0)
                {
                    s.role = Role::Host;
                    if (spec.size() > 5)
                        port = uint16_t(std::atoi(spec.c_str() + 5));
                }
                else if (spec.rfind("join:", 0) == 0)
                {
                    s.role = Role::Client;
                    address = spec.substr(5);
                    if (const size_t colon = address.rfind(':'); colon != std::string::npos)
                    {
                        port = uint16_t(std::atoi(address.c_str() + colon + 1));
                        address.resize(colon);
                    }
                }
                else
                {
                    log("PS2X_NETPLAY must be host[:port] or join:<address>[:port]");
                    return;
                }
                if (s.role == Role::Host)
                {
                    // Players 1-4 sit in the multitap slots of pad port 1.
                    s.players = std::clamp(envInt("PS2X_NETPLAY_PLAYERS", 2), 2, kMaxPlayers);
                    s.delay = std::clamp(envInt("PS2X_NETPLAY_DELAY", 3), 1, 30);
                    s.presentMask = uint8_t((1u << s.players) - 1u);
                    s.hostAddress.sin_port = htons(port);
                    if (!openSocket(port))
                    {
                        log("could not open UDP port " + std::to_string(port));
                        s.role = Role::Off;
                        return;
                    }
                }
                else
                {
                    if (!openSocket(0) || !resolve(address, port, s.hostAddress))
                    {
                        log("could not reach host " + address);
                        s.role = Role::Off;
                        return;
                    }
                }
                if (s.hashEvery == 0u)
                    s.hashEvery = kHashInterval;
                connectSession();
                return;
            }
            if (const char *path = std::getenv("PS2X_INPUT_REPLAY"); path && *path)
            {
                s.file = std::fopen(path, "rb");
                s.role = s.file ? Role::Replay : Role::Off;
                log(s.file ? std::string("replaying inputs from ") + path : std::string("cannot open ") + path);
                return;
            }
            if (const char *path = std::getenv("PS2X_INPUT_RECORD"); path && *path)
            {
                s.file = std::fopen(path, "wb");
                s.role = s.file ? Role::Record : Role::Off;
                log(s.file ? std::string("recording inputs to ") + path : std::string("cannot create ") + path);
                return;
            }
        }

        Role configuredRole()
        {
            static const Role role = []
            {
                if (const char *net = std::getenv("PS2X_NETPLAY"); net && *net)
                    return Role::Host; // any netplay role: the exact one is decided at initialisation
                if (const char *path = std::getenv("PS2X_INPUT_REPLAY"); path && *path)
                    return Role::Replay;
                if (const char *path = std::getenv("PS2X_INPUT_RECORD"); path && *path)
                    return Role::Record;
                return Role::Off;
            }();
            return role;
        }

        uint64_t hashMemory(const uint8_t *rdram)
        {
            constexpr size_t kWords = (32u << 20) / sizeof(uint64_t);
            uint64_t hash = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < kWords; ++i)
            {
                uint64_t word;
                std::memcpy(&word, rdram + i * sizeof(uint64_t), sizeof(word));
                hash = (hash ^ word) * 0x100000001b3ull;
                hash ^= hash >> 29;
            }
            return hash;
        }

        PadInput sampleLocal(PS2Runtime &runtime)
        {
            PadInput in;
            const State &s = state();
            if (s.sampler)
                s.sampler(runtime, in);
            return in;
        }
    }

    bool deterministic()
    {
        return configuredRole() != Role::Off;
    }

    void setMemoryCardAccess(std::filesystem::path (*get)(), void (*set)(const std::filesystem::path &))
    {
        state().getCard = get;
        state().setCard = set;
    }

    void setLocalSampler(bool (*sampler)(PS2Runtime &, PadInput &))
    {
        state().sampler = sampler;
    }

    std::time_t clock(std::time_t hostNow)
    {
        if (!deterministic())
            return hostNow;
        // Every player (and every replay) sees the same date: a fixed start plus emulated time. The game reads the
        // clock during boot, before any session exists, so a host-provided time cannot be used.
        constexpr std::time_t kSessionEpoch = 1767225600; // 2026-01-01 00:00 UTC
        return kSessionEpoch + std::time_t(state().currentTick / 60u);
    }

    bool portInput(int port, PadInput &out)
    {
        const State &s = state();
        if (port < 0 || port >= kMaxPlayers || !(s.presentMask & (1u << port)))
            return false;
        out = s.latched[port];
        return true;
    }

    void onVBlank(PS2Runtime &runtime, const uint8_t *rdram, uint64_t tick)
    {
        if (!deterministic())
            return;
        State &s = state();
        initialize();
        s.currentTick = tick;

        switch (s.role)
        {
        case Role::Record:
        {
            const PadInput in = sampleLocal(runtime);
            s.latched[0] = in;
            uint8_t record[14];
            std::memcpy(record, &tick, 8);
            std::memcpy(record + 8, &in.buttons, 2);
            record[10] = in.rx;
            record[11] = in.ry;
            record[12] = in.lx;
            record[13] = in.ly;
            std::fwrite(record, 1, sizeof(record), s.file);
            if ((tick & 63u) == 0u)
                std::fflush(s.file);
            break;
        }
        case Role::Replay:
        {
            PadInput in;
            uint8_t record[14];
            if (!s.replayEnded && std::fread(record, 1, sizeof(record), s.file) == sizeof(record))
            {
                uint64_t recordedTick;
                std::memcpy(&recordedTick, record, 8);
                if (recordedTick != tick)
                    log("replay tick mismatch: expected " + std::to_string(tick) + ", file has " + std::to_string(recordedTick));
                std::memcpy(&in.buttons, record + 8, 2);
                in.rx = record[10];
                in.ry = record[11];
                in.lx = record[12];
                in.ly = record[13];
            }
            else if (!s.replayEnded)
            {
                s.replayEnded = true;
                log("replay finished at tick " + std::to_string(tick));
            }
            s.latched[0] = in;
            break;
        }
        case Role::Host:
        case Role::Client:
        {
            // This machine's input for tick + delay.
            const uint64_t target = tick + uint64_t(s.delay);
            if (s.localHistory.size() <= target)
                s.localHistory.resize(target + 1u);
            s.localHistory[target] = sampleLocal(runtime);
            Frame &own = s.frames[target];
            own.input[s.localPlayer] = s.localHistory[target];
            own.have |= uint8_t(1u << s.localPlayer);
            pump();
            sendLocalInputs(target);

            // Wait until every player's input for this tick is here (the first `delay` ticks are idle for all).
            if (tick > uint64_t(s.delay))
            {
                using clock = std::chrono::steady_clock;
                const auto start = clock::now();
                auto nextResend = start + std::chrono::milliseconds(50);
                bool warned = false;
                for (;;)
                {
                    const uint8_t needed = uint8_t(s.presentMask & ~s.droppedMask);
                    if ((s.frames[tick].have & needed) == needed)
                        break;
                    pump();
                    const auto now = clock::now();
                    if (now >= nextResend)
                    {
                        sendLocalInputs(target);
                        nextResend = now + std::chrono::milliseconds(50);
                    }
                    if (!warned && now - start > std::chrono::seconds(2))
                    {
                        log("waiting for other players...");
                        warned = true;
                    }
                    if (now - start > std::chrono::seconds(15))
                    {
                        const uint8_t missing = uint8_t(needed & ~s.frames[tick].have);
                        s.droppedMask |= missing;
                        log("lost contact with a player (mask " + std::to_string(missing) + "); their pad is idle from now on");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
            const Frame frame = s.frames[tick];
            for (int p = 0; p < kMaxPlayers; ++p)
                s.latched[p] = (frame.have & (1u << p)) ? frame.input[p] : PadInput{};
            s.frames.erase(s.frames.begin(), s.frames.lower_bound(tick + 1u));
            break;
        }
        case Role::Off:
            return;
        }

        // PS2X_FRAMEDUMP=<tick>[,<tick>...]: write EE RAM to ram_<tick>.bin (determinism debugging).
        if (const char *dump = std::getenv("PS2X_FRAMEDUMP"); dump && rdram)
        {
            const std::string list = std::string(",") + dump + ",";
            if (list.find("," + std::to_string(tick) + ",") != std::string::npos)
            {
                const std::string path = "ram_" + std::to_string(tick) + ".bin";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(rdram, 1, 32u << 20, f);
                    std::fclose(f);
                    log("wrote " + path);
                }
            }
        }

        if (s.hashEvery != 0u && tick % s.hashEvery == 0u && rdram)
        {
            const uint64_t hash = hashMemory(rdram);
            if (std::getenv("PS2X_FRAMEHASH"))
            {
                char text[64];
                std::snprintf(text, sizeof(text), "hash tick %llu %016llx", static_cast<unsigned long long>(tick),
                              static_cast<unsigned long long>(hash));
                log(text);
            }
            if (s.role == Role::Host || s.role == Role::Client)
            {
                s.localHashes[tick] = hash;
                while (s.localHashes.size() > 64u)
                    s.localHashes.erase(s.localHashes.begin());
                if (const auto early = s.remoteHashes.find(tick); early != s.remoteHashes.end())
                    for (const auto &[player, theirs] : early->second)
                        reportDesync(player, tick, theirs, hash);
                s.remoteHashes.erase(s.remoteHashes.begin(), s.remoteHashes.upper_bound(tick));
                Writer w(Hash);
                w.u8(uint8_t(s.localPlayer));
                w.u64(tick);
                w.u64(hash);
                broadcast(w);
            }
        }
    }
}
