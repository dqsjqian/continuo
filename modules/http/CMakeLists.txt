# continuo::http — HTTP/1.1 message parsing and serialisation.
#
# Links only against continuo::core. No OS headers, no socket types: the
# layering check fails the build on either. That is what keeps the parser
# testable over an in-memory buffer and reusable over TLS later.

add_library(continuo_http
    src/message.cpp
    src/parser.cpp
    src/response_parser.cpp
    src/serializer.cpp
)

add_library(continuo::http ALIAS continuo_http)

target_include_directories(continuo_http PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(continuo_http PUBLIC cxx_std_23)
target_link_libraries(continuo_http
    PUBLIC continuo::core
    # See modules/core/CMakeLists.txt for why this is wrapped.
    PRIVATE $<BUILD_INTERFACE:continuo::warnings>
)

set_target_properties(continuo_http PROPERTIES
    OUTPUT_NAME continuo-http
    VERSION ${PROJECT_VERSION}
    # The build-tree ALIAS is continuo::http; without EXPORT_NAME the export
    # uses the real target name and installs continuo::continuo_http instead,
    # so a consumer's link line would differ from an embedder's for no reason.
    EXPORT_NAME http
)

continuo_install_module(continuo_http)

if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
