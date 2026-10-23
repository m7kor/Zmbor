# Runs one end-to-end program and checks both its exit code and the exact line it prints.
# -DRUNNER=<native executable> -DELF=<guest ELF> -DEXPECTED_FILE=<file with the expected stdout line>
file(READ "${EXPECTED_FILE}" expected)
string(STRIP "${expected}" expected)
set(ENV{PS2X_HIDDEN_WINDOW} 1)
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
message(STATUS "end-to-end OK: ${expected}")
