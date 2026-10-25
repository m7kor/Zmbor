# Floating-point build policy guard (docs/architecture/vu-ee-arithmetic-semantics.md, section 4).
# The PS2 arithmetic helpers must give the same bits whatever optimisation flags production uses: the same
# deterministic corpus is run through a strict build and a build with production-like fast flags of
# fp_flag_sensitivity.cpp, and the test fails when any non-[info] line differs. Header-only on purpose: linking
# ps2_runtime would let the linker pick one copy of each inline helper across translation units and hide a
# flag-dependent result.
#   MSVC   : strict /fp:precise            vs /fp:fast /arch:AVX2 (EnableFastReleaseMode's floating-point flags)
#   GCC/Clang: strict -O0                  vs -O3 -mavx2 -mfma (what -march=native gives on current x86), both with
#            the -ffp-contract=off that ps2_runtime imposes PUBLICly. -ffast-math is refused at compile time
#            (negative test below). Whether FMA contraction would change results is not measured here.
if(MSVC)
    set(PS2X_FPENV_STRICT_FLAGS /fp:precise)
    set(PS2X_FPENV_FAST_FLAGS /fp:fast /arch:AVX2)
else()
    set(PS2X_FPENV_STRICT_FLAGS -O0 -msse4.1 -fno-fast-math -ffp-contract=off)
    set(PS2X_FPENV_FAST_FLAGS -O3 -msse4.1 -mavx2 -mfma -ffp-contract=off)
endif()

foreach(variant strict fast)
    add_executable(ps2x_fp_sensitivity_${variant} "${CMAKE_CURRENT_SOURCE_DIR}/fp_flag_sensitivity.cpp")
    target_compile_features(ps2x_fp_sensitivity_${variant} PRIVATE cxx_std_20)
    target_include_directories(ps2x_fp_sensitivity_${variant} PRIVATE
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/ps2xIOP/include"
        $<TARGET_PROPERTY:raylib,INTERFACE_INCLUDE_DIRECTORIES>)
endforeach()
target_compile_options(ps2x_fp_sensitivity_strict PRIVATE ${PS2X_FPENV_STRICT_FLAGS})
target_compile_options(ps2x_fp_sensitivity_fast PRIVATE ${PS2X_FPENV_FAST_FLAGS})

add_test(NAME fpenv_flag_independence
    COMMAND "${CMAKE_COMMAND}"
        -DSTRICT=$<TARGET_FILE:ps2x_fp_sensitivity_strict>
        -DFAST=$<TARGET_FILE:ps2x_fp_sensitivity_fast>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/compare_outputs.cmake")
set_tests_properties(fpenv_flag_independence PROPERTIES TIMEOUT 120 LABELS fpenv)

# Informational compiler probe for the per-lane loop shape that MSVC 14.51 mishandled (not a ctest: production does
# not use the loop; CI prints its result). Built with the default Release flags, like the failing CI build.
add_executable(ps2x_lane_loop_probe "${CMAKE_CURRENT_SOURCE_DIR}/lane_loop_probe.cpp")
target_compile_features(ps2x_lane_loop_probe PRIVATE cxx_std_20)
target_include_directories(ps2x_lane_loop_probe PRIVATE "${CMAKE_SOURCE_DIR}/ps2xRuntime/include")
if(NOT MSVC)
    target_compile_options(ps2x_lane_loop_probe PRIVATE -msse4.1)
endif()

# Negative build test: including the arithmetic headers with -ffast-math must fail to compile.
if(NOT MSVC)
    add_executable(ps2x_fp_fastmath_rejected EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/fp_flag_sensitivity.cpp")
    target_compile_features(ps2x_fp_fastmath_rejected PRIVATE cxx_std_20)
    target_include_directories(ps2x_fp_fastmath_rejected PRIVATE
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/ps2xIOP/include"
        $<TARGET_PROPERTY:raylib,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_options(ps2x_fp_fastmath_rejected PRIVATE -msse4.1 -ffast-math)
    add_test(NAME fpenv_fastmath_rejected
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target ps2x_fp_fastmath_rejected)
    # Passes only when the build fails with the policy's own #error, not on any other failure.
    set_tests_properties(fpenv_fastmath_rejected PROPERTIES
        PASS_REGULAR_EXPRESSION "PS2 arithmetic requires IEEE semantics" TIMEOUT 300 LABELS fpenv)
endif()
