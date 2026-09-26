find_program(MIRA_QUIC_OPENSSL_TOOL NAMES openssl HINTS "${OPENSSL_ROOT_DIR}/bin" REQUIRED)
set(MIRA_QUIC_TEST_CERT "${CMAKE_CURRENT_BINARY_DIR}/cert.pem" CACHE INTERNAL "QUIC 测试证书")
set(MIRA_QUIC_TEST_KEY "${CMAKE_CURRENT_BINARY_DIR}/key.pem" CACHE INTERNAL "QUIC 测试私钥")
execute_process(COMMAND "${MIRA_QUIC_OPENSSL_TOOL}" req -x509 -newkey rsa:2048 -nodes
    -keyout "${MIRA_QUIC_TEST_KEY}" -out "${MIRA_QUIC_TEST_CERT}" -days 2
    -subj /CN=localhost -addext subjectAltName=DNS:localhost
    RESULT_VARIABLE cert_result OUTPUT_QUIET ERROR_QUIET)
if(NOT cert_result EQUAL 0)
    message(FATAL_ERROR "无法生成 QUIC 本地测试证书")
endif()
add_executable(test_quic_engine test_engine.cpp)
target_link_libraries(test_quic_engine PRIVATE Mira::quic)
# 断言 ngtcp2 原生错误码需要其公开头；quic 引擎将依赖保持为 PRIVATE，
# 测试自行携带 include 路径而不把它泄漏给库的消费者。
target_include_directories(test_quic_engine PRIVATE "${MIRA_NGTCP2_INCLUDE_DIR}")
foreach(mode normal chaos bad-host untrusted bad-alpn stream-limit)
    add_test(NAME quic_${mode} COMMAND test_quic_engine "${MIRA_QUIC_TEST_CERT}" "${MIRA_QUIC_TEST_KEY}" ${mode})
    set_tests_properties(quic_${mode} PROPERTIES TIMEOUT 30)
endforeach()

# The engine's in-memory datagram tests prove the protocol state machine;
# this one proves the whole stack over the kernel's UDP.
# GCC 13 hits an internal compiler error (build_special_member_call) on the
# move-only connection coroutine tests; GCC 14+ and Clang compile cleanly.
# Skip on GCC 13 until the runner image moves on — the closing-exchange
# tests report expected failures there.
if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14))
    add_executable(test_quic_udp test_udp_transport.cpp)
    target_link_libraries(test_quic_udp PRIVATE Mira::quic Mira::warnings)
    target_include_directories(test_quic_udp PRIVATE "${CMAKE_SOURCE_DIR}/modules/core/tests")
    add_test(NAME quic_udp COMMAND test_quic_udp "${MIRA_QUIC_TEST_CERT}" "${MIRA_QUIC_TEST_KEY}")
    set_tests_properties(quic_udp PROPERTIES TIMEOUT 60)
endif()
