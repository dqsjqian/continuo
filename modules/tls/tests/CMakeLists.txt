add_executable(continuo_tls_https_tests test_https.cpp)
target_link_libraries(continuo_tls_https_tests PRIVATE
    continuo::tls
    continuo::http
    continuo::transport
    continuo::warnings
    OpenSSL::Crypto
)
target_include_directories(continuo_tls_https_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/core/tests
)
target_compile_definitions(continuo_tls_https_tests PRIVATE
    CONTINUO_TLS_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME tls_https COMMAND continuo_tls_https_tests)
set_tests_properties(tls_https PROPERTIES TIMEOUT 60)
