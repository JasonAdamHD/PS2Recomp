# Native renderer (Vulkan, on the parallel-gs device). Its GLSL shaders are compiled to SPIR-V headers at build
# time with glslangValidator from the Vulkan SDK when present, otherwise with the glslang sources Granite bundles.

set(PS2X_NATIVE_SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/lib/native/shaders")
set(PS2X_NATIVE_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/native_shaders")
file(MAKE_DIRECTORY "${PS2X_NATIVE_SHADER_OUT}")

find_program(PS2X_GLSLANG_VALIDATOR NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(PS2X_GLSLANG_VALIDATOR)
    set(PS2X_GLSLANG_COMMAND "${PS2X_GLSLANG_VALIDATOR}")
    set(PS2X_GLSLANG_DEPENDS "")
else()
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${parallel_gs_SOURCE_DIR}/Granite/third_party/glslang" "${CMAKE_BINARY_DIR}/native_glslang" EXCLUDE_FROM_ALL)
    set(PS2X_GLSLANG_COMMAND "$<TARGET_FILE:glslang-standalone>")
    set(PS2X_GLSLANG_DEPENDS glslang-standalone)
endif()

set(PS2X_NATIVE_SHADER_HEADERS "")
foreach(shader native3d.vert native3d.frag)
    string(REPLACE "." "_" symbol "${shader}")
    set(header "${PS2X_NATIVE_SHADER_OUT}/${symbol}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND ${PS2X_GLSLANG_COMMAND} -V --target-env vulkan1.1 --vn ${symbol} -o "${header}" "${PS2X_NATIVE_SHADER_DIR}/${shader}"
        DEPENDS "${PS2X_NATIVE_SHADER_DIR}/${shader}" ${PS2X_GLSLANG_DEPENDS}
        COMMENT "Compiling native shader ${shader}"
        VERBATIM)
    list(APPEND PS2X_NATIVE_SHADER_HEADERS "${header}")
endforeach()

target_sources(ps2_runtime PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src/lib/native/native_compose.cpp"
    ${PS2X_NATIVE_SHADER_HEADERS}
)
target_include_directories(ps2_runtime PRIVATE "${PS2X_NATIVE_SHADER_OUT}")
