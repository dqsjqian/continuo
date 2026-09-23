add_executable(continuo_core_tests test_core.cpp)

target_link_libraries(continuo_core_tests PRIVATE
    continuo::core
    continuo::warnings
)

target_include_directories(continuo_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME core COMMAND continuo_core_tests)
