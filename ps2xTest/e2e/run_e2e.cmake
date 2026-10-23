# Runs one end-to-end program and checks its exit code and the exact line it prints; with FRAME_EXPECTED it also
# dumps the presented frames (PS2X_DUMP_FRAME) and checks the last one with check_frame.py.
# -DRUNNER=<native executable> -DELF=<guest ELF> -DEXPECTED_FILE=<expected stdout line>
# [-DFRAME_EXPECTED=<frame.json> -DPYTHON=<python> -DCHECK_FRAME=<check_frame.py> -DFRAME_FILE=<frame.ppm>]
file(READ "${EXPECTED_FILE}" expected)
string(STRIP "${expected}" expected)
set(ENV{PS2X_HIDDEN_WINDOW} 1)
if(FRAME_EXPECTED)
    file(REMOVE "${FRAME_FILE}")
    set(ENV{PS2X_DUMP_FRAME} "${FRAME_FILE}")
endif()
execute_process(
    COMMAND "${RUNNER}" "${ELF}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE rc
    TIMEOUT 60)
string(FIND "${out}" "${expected}" found)
if(NOT rc STREQUAL "0" OR found EQUAL -1)
    message(FATAL_ERROR "end-to-end run failed\n  exit code: ${rc}\n  expected line: ${expected}\n"
                        "--- stdout ---\n${out}\n--- stderr (tail) ---\n${err}")
endif()
if(FRAME_EXPECTED)
    if(NOT EXISTS "${FRAME_FILE}")
        message(FATAL_ERROR "end-to-end run presented no frame (no ${FRAME_FILE})")
    endif()
    execute_process(
        COMMAND "${PYTHON}" -I "${CHECK_FRAME}" "${FRAME_FILE}" "${FRAME_EXPECTED}"
        OUTPUT_VARIABLE frame_out
        RESULT_VARIABLE frame_rc)
    message(STATUS "${frame_out}")
    if(NOT frame_rc STREQUAL "0")
        message(FATAL_ERROR "presented frame does not match:\n${frame_out}")
    endif()
endif()
message(STATUS "end-to-end OK: ${expected}")
