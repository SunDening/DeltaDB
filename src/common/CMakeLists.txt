# 公共模块：日志、布隆过滤器、压缩工具、配置等

file(GLOB_RECURSE COMMON_SOURCES *.cpp)

add_library(db_common STATIC ${COMMON_SOURCES})

target_include_directories(db_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(db_common
    PUBLIC
        spdlog::spdlog
    PRIVATE
        xxhash::xxhash
        Snappy::snappy
        ZLIB::ZLIB
)

add_library(DeltaDB::common ALIAS db_common)