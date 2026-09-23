add_executable(continuo_http_tests test_parser.cpp)

target_link_libraries(continuo_http_tests PRIVATE
    continuo::http
    continuo::warnings
)

# Reuse the core test harness rather than duplicating it.
target_include_directories(continuo_http_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/modules/core/tests
)

add_test(NAME http_parser COMMAND continuo_http_tests)
