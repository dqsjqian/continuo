# continuo::core — event loop, buffers, coroutine plumbing, error model.
#
# A compiled library, not header-only. Header-only was considered and rejected:
# the reference point for this project ships 22k lines in one header, and the
# compile-time cost of that lands on every translation unit of every consumer.
# A compiled target also gives Continuo somewhere to put a real ABI policy.

add_library(continuo_core
    src/error.cpp
)

add_library(continuo::core ALIAS continuo_core)

target_include_directories(continuo_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(continuo_core PUBLIC cxx_std_20)
target_link_libraries(continuo_core PRIVATE continuo::warnings)

set_target_properties(continuo_core PROPERTIES
    OUTPUT_NAME continuo-core
    VERSION ${PROJECT_VERSION}
)
