find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_library(continuo_tls
    src/context.cpp
    src/engine.cpp
    src/error.cpp
)
add_library(continuo::tls ALIAS continuo_tls)

target_include_directories(continuo_tls PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(continuo_tls PUBLIC cxx_std_23)
target_link_libraries(continuo_tls
    PUBLIC continuo::core OpenSSL::SSL OpenSSL::Crypto
    # See modules/core/CMakeLists.txt for why this is wrapped.
    PRIVATE $<BUILD_INTERFACE:continuo::warnings>
)
set_target_properties(continuo_tls PROPERTIES
    OUTPUT_NAME continuo-tls
    VERSION ${PROJECT_VERSION}
    # The build-tree ALIAS is continuo::tls; without EXPORT_NAME the export
    # uses the real target name and installs continuo::continuo_tls instead,
    # so a consumer's link line would differ from an embedder's for no reason.
    EXPORT_NAME tls
)

continuo_install_module(continuo_tls)

if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
