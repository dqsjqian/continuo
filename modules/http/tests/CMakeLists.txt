add_executable(continuo_http_tests test_parser.cpp)
add_executable(continuo_http_server_tests test_server.cpp)

# The only binary that links both http and transport. The *library* keeps its
# boundary (continuo::http depends on core alone); a test that runs the whole
# stack over a real socket is exactly where the layers should be checked
# against each other.
add_executable(continuo_http_e2e_tests test_end_to_end.cpp)
target_link_libraries(continuo_http_e2e_tests PRIVATE continuo::transport)

foreach(target continuo_http_tests continuo_http_server_tests continuo_http_e2e_tests)
    target_link_libraries(${target} PRIVATE
        continuo::http
        continuo::warnings
    )
    # Reuse the core test harness rather than duplicating it.
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/modules/core/tests
    )
endforeach()

add_test(NAME http_parser COMMAND continuo_http_tests)
add_test(NAME http_server COMMAND continuo_http_server_tests)
add_test(NAME http_end_to_end COMMAND continuo_http_e2e_tests)

# A connection-loop bug hangs rather than fails; a timeout makes it red.
set_tests_properties(http_server http_end_to_end PROPERTIES TIMEOUT 60)
