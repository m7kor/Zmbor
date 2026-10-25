# Micro-benchmarks (docs/architecture/benchmark-methodology.md). Built so they cannot rot; never run as tests.
# Header-only like tools/fpenv: the benchmark sees the same inline helpers the recompiled code does.
add_executable(ps2x_bench_vu_fmac "${CMAKE_CURRENT_SOURCE_DIR}/vu_fmac_bench.cpp")
target_compile_features(ps2x_bench_vu_fmac PRIVATE cxx_std_20)
target_include_directories(ps2x_bench_vu_fmac PRIVATE
    "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
    "${CMAKE_SOURCE_DIR}/ps2xIOP/include"
    $<TARGET_PROPERTY:raylib,INTERFACE_INCLUDE_DIRECTORIES>)
if(NOT MSVC)
    target_compile_options(ps2x_bench_vu_fmac PRIVATE -msse4.1)
endif()

# VU1 interpreter workload (links the runtime; see vu1_interp_bench.cpp for the program and scenarios).
add_executable(ps2x_bench_vu1 "${CMAKE_CURRENT_SOURCE_DIR}/vu1_interp_bench.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sampling_profiler.cpp")
if(WIN32)
    target_link_libraries(ps2x_bench_vu1 PRIVATE dbghelp)
endif()
target_link_libraries(ps2x_bench_vu1 PRIVATE ps2_runtime)
if(COMMAND ps2x_link_like_runtime)
    ps2x_link_like_runtime(ps2x_bench_vu1)
endif()
