# End-to-end tests: a guest ELF is assembled from source (e2e_programs.py), recompiled by ps2_recomp, compiled
# with the runtime into a native executable, run, and its observable output checked. Needs no PS2 toolchain.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT TARGET ps2_recomp OR NOT TARGET ps2_runtime)
    message(STATUS "ps2xTest e2e: skipped (needs Python 3, ps2_recomp and ps2_runtime)")
    return()
endif()

set(PS2X_E2E_SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Release builds compile ps2_runtime with IPO (ReleaseMode.cmake). With Clang its archive then holds LLVM bitcode,
# which GNU ld cannot read unless the executable links with LTO too ("libps2_runtime.a: error adding symbols: file
# format not recognized" on CI linux-clang). GCC loads its LTO plugin on every link, and MSVC links /GL libraries
# without /LTCG, so only non-MSVC executables need it.
function(ps2x_link_like_runtime target)
    get_target_property(runtime_ipo ps2_runtime INTERPROCEDURAL_OPTIMIZATION_RELEASE)
    if(runtime_ipo AND NOT MSVC)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()

function(ps2x_add_e2e_program name)
    cmake_parse_arguments(E2E "FRAME" "" "" ${ARGN})
    set(work "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    set(gen "${work}/${name}_generated")
    set(generated
        "${gen}/ps2_recompiled_functions.cpp"
        "${gen}/register_functions.cpp"
        "${gen}/ps2_recompiled_functions.h"
        "${gen}/ps2_recompiled_stubs.h")

    add_custom_command(
        OUTPUT ${generated} "${work}/${name}.elf" "${work}/${name}.expected"
        COMMAND "${Python3_EXECUTABLE}" -I "${PS2X_E2E_SCRIPT_DIR}/e2e_programs.py" ${name} "${work}"
        COMMAND $<TARGET_FILE:ps2_recomp> "${work}/${name}.toml"
        DEPENDS "${PS2X_E2E_SCRIPT_DIR}/e2e_programs.py" "${PS2X_E2E_SCRIPT_DIR}/mips_elf.py"
                "${CMAKE_SOURCE_DIR}/tools/semantics/vu_fmac_model.py" ps2_recomp
        BYPRODUCTS "${work}/${name}.frame.json"
        COMMENT "e2e: assembling and recompiling ${name}"
        VERBATIM)

    add_executable(ps2x_e2e_${name}
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/main.cpp"
        "${gen}/ps2_recompiled_functions.cpp"
        "${gen}/register_functions.cpp")
    target_include_directories(ps2x_e2e_${name} PRIVATE "${gen}")
    target_link_libraries(ps2x_e2e_${name} PRIVATE ps2_runtime)
    ps2x_link_like_runtime(ps2x_e2e_${name})
    if(MSVC)
        # Same WinAPI/raylib CloseWindow clash workaround as ps2EntryRunner.
        target_link_options(ps2x_e2e_${name} PRIVATE "/FORCE:MULTIPLE")
    endif()
    if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        ps2x_stage_ffmpeg_runtime_dlls(ps2x_e2e_${name})
    endif()

    set(frame_args "")
    if(E2E_FRAME)
        set(frame_args
            -DFRAME_EXPECTED=${work}/${name}.frame.json
            -DFRAME_FILE=${work}/${name}.frame.ppm
            -DPYTHON=${Python3_EXECUTABLE}
            -DCHECK_FRAME=${PS2X_E2E_SCRIPT_DIR}/check_frame.py)
    endif()
    add_test(NAME e2e_${name}
        COMMAND "${CMAKE_COMMAND}"
            -DRUNNER=$<TARGET_FILE:ps2x_e2e_${name}>
            -DELF=${work}/${name}.elf
            -DEXPECTED_FILE=${work}/${name}.expected
            ${frame_args}
            -P "${PS2X_E2E_SCRIPT_DIR}/run_e2e.cmake")
    set_tests_properties(e2e_${name} PROPERTIES TIMEOUT 90 LABELS e2e)
endfunction()

ps2x_add_e2e_program(basic)
ps2x_add_e2e_program(gs_sprite FRAME)
ps2x_add_e2e_program(branchdelay)
ps2x_add_e2e_program(vu_macro)

# Hardware result tests: ELFs recompiled by tools/hwtest/hwtest.py prepare (for example ps2autotests, whose
# .expected files were printed by a real PS2). Configure with -DPS2X_HWTEST_GENERATED=<prepared dir>.
set(PS2X_HWTEST_GENERATED "" CACHE PATH "Directory prepared by tools/hwtest/hwtest.py prepare")
# Optimizing the multi-megabyte recompiled sources costs minutes per test (MSVC /O2), against seconds unoptimized;
# the generated code's semantics do not depend on it (x64 SSE math, /fp:precise either way). The runtime keeps the
# build type's optimization. Turn this on for a run that matches shipped builds.
option(PS2X_HWTEST_OPTIMIZE "Optimize recompiled code of hardware result tests" OFF)
if(PS2X_HWTEST_GENERATED)
    file(GLOB ps2x_hw_manifests "${PS2X_HWTEST_GENERATED}/*/hwtest.cmake")
    foreach(ps2x_hw_manifest IN LISTS ps2x_hw_manifests)
        include("${ps2x_hw_manifest}") # HW_NAME, HW_ELF, HW_EXPECTED, HW_CONFIG, HW_GEN, HW_RESULT
        # Recompiled at build time, so a recompiler change regenerates every hardware test.
        set(ps2x_hw_sources "${HW_GEN}/ps2_recompiled_functions.cpp" "${HW_GEN}/register_functions.cpp")
        add_custom_command(
            OUTPUT ${ps2x_hw_sources} "${HW_GEN}/ps2_recompiled_functions.h" "${HW_GEN}/ps2_recompiled_stubs.h"
            COMMAND $<TARGET_FILE:ps2_recomp> "${HW_CONFIG}"
            DEPENDS ps2_recomp "${HW_ELF}" "${HW_CONFIG}"
            COMMENT "hwtest: recompiling ${HW_NAME}"
            VERBATIM)
        add_executable(ps2x_hw_${HW_NAME} "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/main.cpp" ${ps2x_hw_sources})
        target_include_directories(ps2x_hw_${HW_NAME} PRIVATE "${HW_GEN}")
        target_link_libraries(ps2x_hw_${HW_NAME} PRIVATE ps2_runtime)
        ps2x_link_like_runtime(ps2x_hw_${HW_NAME})
        if(NOT PS2X_HWTEST_OPTIMIZE)
            set_source_files_properties(${ps2x_hw_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/Od,-O0>")
        endif()
        if(MSVC)
            # Unoptimized recompiled functions keep every local in its own stack slot (frames up to ~700 KB were
            # measured in vu/lower/efu), so a few nested guest calls overflowed the 1 MB default. Threads take the
            # image's reserve; 64 MB is reserved, not committed.
            target_link_options(ps2x_hw_${HW_NAME} PRIVATE "/STACK:67108864")
            target_link_options(ps2x_hw_${HW_NAME} PRIVATE "/FORCE:MULTIPLE")
            target_compile_options(ps2x_hw_${HW_NAME} PRIVATE /bigobj)
        endif()
        add_test(NAME hw_${HW_NAME}
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/hwtest/hwtest.py" run
                --runner $<TARGET_FILE:ps2x_hw_${HW_NAME}> --elf "${HW_ELF}" --expected "${HW_EXPECTED}"
                --result "${HW_RESULT}")
        set_tests_properties(hw_${HW_NAME} PROPERTIES TIMEOUT 120 LABELS hwtest)
    endforeach()
endif()
