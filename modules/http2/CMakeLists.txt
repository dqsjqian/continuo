# 已验证依赖：nghttp2 1.70.0，MIT（上游 COPYING）。不触发网络下载。
# 官方发布 tar.xz SHA256: e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
# v1.70.0 commit: 85e300c79fb6dbcfa9c1013215c8710c1c2cd3d2
# 1.61 是所需 *2 / max_continuations API 下限，不等于其他版本已完成运行验证。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(NGHTTP2 1.61 REQUIRED)

add_library(mira_http2 src/session.cpp)
add_library(Mira::http2 ALIAS mira_http2)
target_include_directories(mira_http2 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mira_http2 PUBLIC cxx_std_23)
target_link_libraries(mira_http2 PUBLIC Mira::core
    PRIVATE NGHTTP2::NGHTTP2 $<BUILD_INTERFACE:Mira::warnings>)
set_target_properties(mira_http2 PROPERTIES OUTPUT_NAME Mira-http2
    VERSION ${PROJECT_VERSION} EXPORT_NAME http2)
mira_install_module(mira_http2)
if(MIRA_INSTALL)
    install(FILES cmake/FindNGHTTP2.cmake cmake/Mira-http2-dependencies.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mira)
endif()
if(MIRA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
