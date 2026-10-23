# End-to-end tests: a guest ELF is assembled from source (e2e_programs.py), recompiled by ps2_recomp, compiled
# with the runtime into a native executable, run, and its observable output checked. Needs no PS2 toolchain.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT TARGET ps2_recomp OR NOT TARGET ps2_runtime)
    message(STATUS "ps2xTest e2e: skipped (needs Python 3, ps2_recomp and ps2_runtime)")
    return()
endif()

set(PS2X_E2E_SCRIPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(ps2x_add_e2e_program name)
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
        DEPENDS "${PS2X_E2E_SCRIPT_DIR}/e2e_programs.py" "${PS2X_E2E_SCRIPT_DIR}/mips_elf.py" ps2_recomp
        COMMENT "e2e: assembling and recompiling ${name}"
        VERBATIM)

    add_executable(ps2x_e2e_${name}
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/main.cpp"
        "${gen}/ps2_recompiled_functions.cpp"
        "${gen}/register_functions.cpp")
    target_include_directories(ps2x_e2e_${name} PRIVATE "${gen}")
    target_link_libraries(ps2x_e2e_${name} PRIVATE ps2_runtime)
    if(MSVC)
        # Same WinAPI/raylib CloseWindow clash workaround as ps2EntryRunner.
        target_link_options(ps2x_e2e_${name} PRIVATE "/FORCE:MULTIPLE")
    endif()
    if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        ps2x_stage_ffmpeg_runtime_dlls(ps2x_e2e_${name})
    endif()

    add_test(NAME e2e_${name}
        COMMAND "${CMAKE_COMMAND}"
            -DRUNNER=$<TARGET_FILE:ps2x_e2e_${name}>
            -DELF=${work}/${name}.elf
            -DEXPECTED_FILE=${work}/${name}.expected
            -P "${PS2X_E2E_SCRIPT_DIR}/run_e2e.cmake")
    set_tests_properties(e2e_${name} PROPERTIES TIMEOUT 90 LABELS e2e)
endfunction()

ps2x_add_e2e_program(basic)
