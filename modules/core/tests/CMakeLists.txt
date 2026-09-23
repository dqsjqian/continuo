# One executable per subject area, so a failure names the area it broke.

add_executable(continuo_core_tests test_core.cpp)
add_executable(continuo_event_loop_tests test_event_loop.cpp)

foreach(target continuo_core_tests continuo_event_loop_tests)
    target_link_libraries(${target} PRIVATE
        continuo::core
        continuo::warnings
    )
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

add_test(NAME core COMMAND continuo_core_tests)
add_test(NAME event_loop COMMAND continuo_event_loop_tests)

# An event-loop bug tends to hang rather than fail. A timeout turns that into
# a red test instead of a stuck CI job.
set_tests_properties(event_loop PROPERTIES TIMEOUT 60)
