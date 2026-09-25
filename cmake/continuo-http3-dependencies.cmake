include(CMakeFindDependencyMacro)
find_dependency(OpenSSL 3.5 COMPONENTS SSL Crypto)
find_path(CONTINUO_NGTCP2_INCLUDE_DIR ngtcp2/ngtcp2_crypto_ossl.h REQUIRED)
find_library(CONTINUO_NGTCP2_LIBRARY NAMES ngtcp2 REQUIRED)
find_library(CONTINUO_NGTCP2_OSSL_LIBRARY NAMES ngtcp2_crypto_ossl REQUIRED)
find_path(CONTINUO_NGHTTP3_INCLUDE_DIR nghttp3/nghttp3.h REQUIRED)
find_library(CONTINUO_NGHTTP3_LIBRARY NAMES nghttp3 REQUIRED)
if(NOT TARGET continuo_ngtcp2)
    add_library(continuo_ngtcp2 UNKNOWN IMPORTED)
    set_target_properties(continuo_ngtcp2 PROPERTIES
        IMPORTED_LOCATION "${CONTINUO_NGTCP2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CONTINUO_NGTCP2_INCLUDE_DIR}")
endif()
if(NOT TARGET continuo_ngtcp2_ossl)
    add_library(continuo_ngtcp2_ossl UNKNOWN IMPORTED)
    set_target_properties(continuo_ngtcp2_ossl PROPERTIES
        IMPORTED_LOCATION "${CONTINUO_NGTCP2_OSSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CONTINUO_NGTCP2_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "continuo_ngtcp2;OpenSSL::SSL;OpenSSL::Crypto")
endif()
if(NOT TARGET continuo_nghttp3)
    add_library(continuo_nghttp3 UNKNOWN IMPORTED)
    set_target_properties(continuo_nghttp3 PROPERTIES
        IMPORTED_LOCATION "${CONTINUO_NGHTTP3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CONTINUO_NGHTTP3_INCLUDE_DIR}")
endif()
