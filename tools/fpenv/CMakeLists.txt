# Floating-point build policy guard (docs/architecture/vu-ee-arithmetic-semantics.md, section 4).
# The PS2 arithmetic helpers must give the same bits whatever the host compiler's floating-point mode: the same
# deterministic corpus is run through a strict build and a fast-math build of fp_flag_sensitivity.cpp, and the test
# fails when any non-[info] line differs. Header-only on purpose: linking ps2_runtime would let the linker pick one
# copy of each inline helper across translation units and hide a flag-dependent result.
set(PS2X_FPENV_STRICT_FLAGS "$<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-fno-fast-math>")
set(PS2X_FPENV_FAST_FLAGS "$<IF:$<CXX_COMPILER_ID:MSVC>,/fp:fast,-ffast-math>")

foreach(variant strict fast)
    add_executable(ps2x_fp_sensitivity_${variant} "${CMAKE_CURRENT_SOURCE_DIR}/fp_flag_sensitivity.cpp")
    target_compile_features(ps2x_fp_sensitivity_${variant} PRIVATE cxx_std_20)
    target_include_directories(ps2x_fp_sensitivity_${variant} PRIVATE
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/ps2xIOP/include"
        $<TARGET_PROPERTY:raylib,INTERFACE_INCLUDE_DIRECTORIES>)
    if(NOT MSVC)
        target_compile_options(ps2x_fp_sensitivity_${variant} PRIVATE -msse4.1)
    endif()
endforeach()
target_compile_options(ps2x_fp_sensitivity_strict PRIVATE ${PS2X_FPENV_STRICT_FLAGS})
target_compile_options(ps2x_fp_sensitivity_fast PRIVATE ${PS2X_FPENV_FAST_FLAGS})

add_test(NAME fpenv_flag_independence
    COMMAND "${CMAKE_COMMAND}"
        -DSTRICT=$<TARGET_FILE:ps2x_fp_sensitivity_strict>
        -DFAST=$<TARGET_FILE:ps2x_fp_sensitivity_fast>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/compare_outputs.cmake")
set_tests_properties(fpenv_flag_independence PROPERTIES TIMEOUT 120 LABELS fpenv)
