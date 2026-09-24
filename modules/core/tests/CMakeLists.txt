# One executable per subject area, so a failure names the area it broke.

add_executable(continuo_core_tests test_core.cpp)
add_executable(continuo_event_loop_tests test_event_loop.cpp)

add_executable(continuo_task_scope_tests test_task_scope.cpp)

foreach(target continuo_core_tests continuo_event_loop_tests continuo_task_scope_tests)
    target_link_libraries(${target} PRIVATE
        continuo::core
        continuo::warnings
    )
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

add_test(NAME core COMMAND continuo_core_tests)
add_test(NAME event_loop COMMAND continuo_event_loop_tests)
add_test(NAME task_scope COMMAND continuo_task_scope_tests)
set_tests_properties(task_scope PROPERTIES TIMEOUT 60)

# Contract violations must terminate, and only a separate process can assert
# that. `check_terminates.cmake` demands the exact exit code the binary's
# terminate handler installs, so an ordinary crash does not pass as fail-fast.
function(continuo_add_terminate_tests prefix target)
    foreach(mode ${ARGN})
        add_test(NAME ${prefix}_${mode} COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:${target}> -DMODE=${mode}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_terminates.cmake)
        set_tests_properties(${prefix}_${mode} PROPERTIES TIMEOUT 15)
    endforeach()
endfunction()

continuo_add_terminate_tests(task_scope continuo_task_scope_tests
    pending-destruction unobserved-failure abandoned-join unstarted-join)

continuo_add_terminate_tests(task_contract continuo_core_tests
    sync-get-suspended abandoned-awaiter)

continuo_add_terminate_tests(event_loop continuo_event_loop_tests
    destroy-during-dispatch reentrant-run-once)

# An event-loop bug tends to hang rather than fail. A timeout turns that into
# a red test instead of a stuck CI job.
set_tests_properties(event_loop PROPERTIES TIMEOUT 60)
