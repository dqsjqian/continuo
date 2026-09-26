include(CMakeFindDependencyMacro)
find_dependency(OpenSSL 3.5 COMPONENTS SSL Crypto)
find_path(MIRA_NGTCP2_INCLUDE_DIR ngtcp2/ngtcp2_crypto_ossl.h REQUIRED)
find_library(MIRA_NGTCP2_LIBRARY NAMES ngtcp2 REQUIRED)
find_library(MIRA_NGTCP2_OSSL_LIBRARY NAMES ngtcp2_crypto_ossl REQUIRED)
find_path(MIRA_NGHTTP3_INCLUDE_DIR nghttp3/nghttp3.h REQUIRED)
find_library(MIRA_NGHTTP3_LIBRARY NAMES nghttp3 REQUIRED)
if(NOT TARGET mira_ngtcp2)
    add_library(mira_ngtcp2 UNKNOWN IMPORTED)
    set_target_properties(mira_ngtcp2 PROPERTIES
        IMPORTED_LOCATION "${MIRA_NGTCP2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MIRA_NGTCP2_INCLUDE_DIR}")
endif()
if(NOT TARGET mira_ngtcp2_ossl)
    add_library(mira_ngtcp2_ossl UNKNOWN IMPORTED)
    set_target_properties(mira_ngtcp2_ossl PROPERTIES
        IMPORTED_LOCATION "${MIRA_NGTCP2_OSSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MIRA_NGTCP2_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "mira_ngtcp2;OpenSSL::SSL;OpenSSL::Crypto")
endif()
if(NOT TARGET mira_nghttp3)
    add_library(mira_nghttp3 UNKNOWN IMPORTED)
    set_target_properties(mira_nghttp3 PROPERTIES
        IMPORTED_LOCATION "${MIRA_NGHTTP3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MIRA_NGHTTP3_INCLUDE_DIR}")
endif()
