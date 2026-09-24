# Assert that a contract violation terminates, with the exact exit code the
# test binary's terminate handler installs.
#
# Exactness matters: any crash produces *some* non-zero status, so accepting
# "it failed somehow" would let a segfault pass as a deliberate fail-fast.
if(NOT DEFINED PROGRAM OR NOT DEFINED MODE)
    message(FATAL_ERROR "PROGRAM and MODE are required")
endif()
execute_process(COMMAND "${PROGRAM}" "${MODE}" RESULT_VARIABLE result TIMEOUT 10)
if(NOT "${result}" STREQUAL "77")
    message(FATAL_ERROR "Contract ${MODE} expected terminate handler exit 77, got ${result}")
endif()
