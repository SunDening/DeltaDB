file(GLOB_RECURSE WAL_SOURCES *.cpp)

add_library(db_wal STATIC ${WAL_SOURCES})

target_include_directories(db_wal
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        # wal 实现可能需要 kv 的头文件，但接口不暴露
        ${CMAKE_SOURCE_DIR}/src/kv/include
)

target_link_libraries(db_wal
    PUBLIC
        DeltaDB::common
        xxhash::xxhash
    PRIVATE
        ZLIB::ZLIB
)

add_library(DeltaDB::wal ALIAS db_wal)