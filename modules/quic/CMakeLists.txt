# 已验证依赖：ngtcp2 1.22.1（MIT），OpenSSL 3.6.2（Apache-2.0）。
# ngtcp2-1.22.1.tar.xz SHA256:
# dfd2c68bd64b89847c611425b9487105c46e8447b5c21e6aeb00642c8fbe2ca8
# 官方源码：https://github.com/ngtcp2/ngtcp2/releases/download/v1.22.1/ngtcp2-1.22.1.tar.xz
# ossl QUIC TLS adapter 仍为上游 experimental；不自动下载依赖。
find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)
find_path(CONTINUO_NGTCP2_INCLUDE_DIR ngtcp2/ngtcp2_crypto_ossl.h REQUIRED)
find_library(CONTINUO_NGTCP2_LIBRARY NAMES ngtcp2 REQUIRED)
find_library(CONTINUO_NGTCP2_OSSL_LIBRARY NAMES ngtcp2_crypto_ossl REQUIRED)
if(NOT TARGET continuo_ngtcp2)
    add_library(continuo_ngtcp2 UNKNOWN IMPORTED GLOBAL)
    set_target_properties(continuo_ngtcp2 PROPERTIES IMPORTED_LOCATION "${CONTINUO_NGTCP2_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${CONTINUO_NGTCP2_INCLUDE_DIR}")
    add_library(continuo_ngtcp2_ossl UNKNOWN IMPORTED GLOBAL)
    set_target_properties(continuo_ngtcp2_ossl PROPERTIES IMPORTED_LOCATION "${CONTINUO_NGTCP2_OSSL_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${CONTINUO_NGTCP2_INCLUDE_DIR}" INTERFACE_LINK_LIBRARIES "continuo_ngtcp2;OpenSSL::SSL;OpenSSL::Crypto")
endif()
add_library(continuo_quic src/engine.cpp)
add_library(continuo::quic ALIAS continuo_quic)
target_include_directories(continuo_quic PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(continuo_quic PUBLIC cxx_std_23)
target_link_libraries(continuo_quic PUBLIC continuo::transport PRIVATE continuo_ngtcp2_ossl $<BUILD_INTERFACE:continuo::warnings>)
set_target_properties(continuo_quic PROPERTIES OUTPUT_NAME continuo-quic EXPORT_NAME quic)
if(COMMAND continuo_install_module)
    continuo_install_module(continuo_quic)
endif()
if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
