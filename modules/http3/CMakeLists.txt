# 已验证依赖：nghttp3 1.15.0（MIT），不自动下载依赖。
# nghttp3-1.15.0.tar.xz SHA256:
# 6da0cd06b428d32a54c58137838505d9dc0371a900bb8070a46b29e1ceaf2e0f
# 官方源码：https://github.com/ngtcp2/nghttp3/releases/download/v1.15.0/nghttp3-1.15.0.tar.xz
find_path(CONTINUO_NGHTTP3_INCLUDE_DIR nghttp3/nghttp3.h REQUIRED)
find_library(CONTINUO_NGHTTP3_LIBRARY NAMES nghttp3 REQUIRED)
if(NOT TARGET continuo_nghttp3)
    add_library(continuo_nghttp3 UNKNOWN IMPORTED GLOBAL)
    set_target_properties(continuo_nghttp3 PROPERTIES IMPORTED_LOCATION "${CONTINUO_NGHTTP3_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${CONTINUO_NGHTTP3_INCLUDE_DIR}")
endif()
add_library(continuo_http3 src/engine.cpp)
add_library(continuo::http3 ALIAS continuo_http3)
target_include_directories(continuo_http3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(continuo_http3 PUBLIC cxx_std_23)
# 加密随机源由 continuo::quic 提供；本模块不直接依赖 TLS 后端。
target_link_libraries(continuo_http3 PUBLIC continuo::quic PRIVATE continuo_nghttp3 $<BUILD_INTERFACE:continuo::warnings>)
set_target_properties(continuo_http3 PROPERTIES OUTPUT_NAME continuo-http3 EXPORT_NAME http3)
if(COMMAND continuo_install_module)
    continuo_install_module(continuo_http3)
endif()
if(CONTINUO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
