# continuo::http — HTTP/1.1 message parsing and serialisation.
#
# Links only against continuo::core. No OS headers, no socket types: the
# layering check fails the build on either. That is what keeps the parser
# testable over an in-memory buffer and reusable over TLS later.

add_library(continuo_http
    src/message.cpp
    src/parser.cpp
    src/serializer.cpp
)

add_library(continuo::http ALIAS continuo_http)

target_include_directories(continuo_http PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(continuo_http PUBLIC cxx_std_20)
target_link_libraries(continuo_http
    PUBLIC continuo::core
    PRIVATE continuo::warnings
)

set_target_properties(continuo_http PROPERTIES
    OUTPUT_NAME continuo-http
    VERSION ${PROJECT_VERSION}
)

if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
