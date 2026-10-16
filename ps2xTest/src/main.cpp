#include "MiniTest.h"
#include <cstdlib>
#include <iostream>
#include <string>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_vu0_flag_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void reset_ps2_test_function_table();

int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    // PS2X_TEST_ONLY=vu0 runs just the VU0/FPU/MMI semantics tests.
    if (const char *only = std::getenv("PS2X_TEST_ONLY"); only && std::string(only) == "vu0")
    {
        register_vu0_flag_tests();
        int res = MiniTest::Run();
        std::cout.flush();
        std::_Exit(res);
    }

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_vu0_flag_tests();
    register_ps2_gs_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
