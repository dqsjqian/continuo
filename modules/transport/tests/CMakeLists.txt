add_executable(continuo_transport_tests test_tcp.cpp)

target_link_libraries(continuo_transport_tests PRIVATE
    continuo::transport
    continuo::warnings
)

target_include_directories(continuo_transport_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/modules/core/tests
)

add_test(NAME transport COMMAND continuo_transport_tests)

# A transport bug hangs rather than fails; a timeout makes it red instead.
set_tests_properties(transport PROPERTIES TIMEOUT 60)

add_executable(continuo_udp_tests test_udp.cpp)
target_link_libraries(continuo_udp_tests PRIVATE continuo::transport continuo::warnings)
target_include_directories(continuo_udp_tests PRIVATE ${CMAKE_SOURCE_DIR}/modules/core/tests)
add_test(NAME udp COMMAND continuo_udp_tests)
set_tests_properties(udp PROPERTIES TIMEOUT 60)

add_executable(continuo_resolver_tests test_resolver.cpp)
target_link_libraries(continuo_resolver_tests PRIVATE continuo::transport continuo::warnings)
target_include_directories(continuo_resolver_tests PRIVATE ${CMAKE_SOURCE_DIR}/modules/core/tests)
add_test(NAME resolver COMMAND continuo_resolver_tests)
set_tests_properties(resolver PROPERTIES TIMEOUT 60)
