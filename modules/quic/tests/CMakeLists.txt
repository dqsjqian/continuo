find_program(CONTINUO_QUIC_OPENSSL_TOOL NAMES openssl HINTS "${OPENSSL_ROOT_DIR}/bin" REQUIRED)
set(CONTINUO_QUIC_TEST_CERT "${CMAKE_CURRENT_BINARY_DIR}/cert.pem" CACHE INTERNAL "QUIC 测试证书")
set(CONTINUO_QUIC_TEST_KEY "${CMAKE_CURRENT_BINARY_DIR}/key.pem" CACHE INTERNAL "QUIC 测试私钥")
execute_process(COMMAND "${CONTINUO_QUIC_OPENSSL_TOOL}" req -x509 -newkey rsa:2048 -nodes
    -keyout "${CONTINUO_QUIC_TEST_KEY}" -out "${CONTINUO_QUIC_TEST_CERT}" -days 2
    -subj /CN=localhost -addext subjectAltName=DNS:localhost
    RESULT_VARIABLE cert_result OUTPUT_QUIET ERROR_QUIET)
if(NOT cert_result EQUAL 0)
    message(FATAL_ERROR "无法生成 QUIC 本地测试证书")
endif()
add_executable(test_quic_engine test_engine.cpp)
target_link_libraries(test_quic_engine PRIVATE continuo::quic)
# 断言 ngtcp2 原生错误码需要其公开头；quic 引擎将依赖保持为 PRIVATE，
# 测试自行携带 include 路径而不把它泄漏给库的消费者。
target_include_directories(test_quic_engine PRIVATE "${CONTINUO_NGTCP2_INCLUDE_DIR}")
foreach(mode normal chaos bad-host untrusted bad-alpn stream-limit)
    add_test(NAME quic_${mode} COMMAND test_quic_engine "${CONTINUO_QUIC_TEST_CERT}" "${CONTINUO_QUIC_TEST_KEY}" ${mode})
    set_tests_properties(quic_${mode} PROPERTIES TIMEOUT 30)
endforeach()
