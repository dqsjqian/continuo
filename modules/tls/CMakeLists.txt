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
    PRIVATE continuo::warnings
)
set_target_properties(continuo_tls PROPERTIES
    OUTPUT_NAME continuo-tls
    VERSION ${PROJECT_VERSION}
)

if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
