# continuo::transport — TCP listeners, connections, and endpoints.
#
# The only module besides core/ that touches OS networking headers, and it
# confines them to src/socket_compat.hpp. Protocols never link against this:
# they take an AsyncStream, and tcp::Socket happens to model it.

add_library(continuo_transport
    src/endpoint.cpp
    src/tcp.cpp
)

add_library(continuo::transport ALIAS continuo_transport)

target_include_directories(continuo_transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(continuo_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(continuo_transport PUBLIC cxx_std_23)
target_link_libraries(continuo_transport
    PUBLIC continuo::core
    # See modules/core/CMakeLists.txt for why this is wrapped.
    PRIVATE $<BUILD_INTERFACE:continuo::warnings>
)

if(WIN32)
    target_link_libraries(continuo_transport PUBLIC ws2_32 mswsock)
endif()

set_target_properties(continuo_transport PROPERTIES
    OUTPUT_NAME continuo-transport
    VERSION ${PROJECT_VERSION}
    # The build-tree ALIAS is continuo::transport; without EXPORT_NAME the export
    # uses the real target name and installs continuo::continuo_transport instead,
    # so a consumer's link line would differ from an embedder's for no reason.
    EXPORT_NAME transport
)

continuo_install_module(continuo_transport)

if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
