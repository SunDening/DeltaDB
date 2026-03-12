file(GLOB_RECURSE SST_SOURCES *.cpp)

add_library(db_sst STATIC ${SST_SOURCES})

target_include_directories(db_sst
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        # sst 实现需要 wal 和 kv 的头文件
        ${CMAKE_SOURCE_DIR}/src/wal/include
        ${CMAKE_SOURCE_DIR}/src/kv/include
)

target_link_libraries(db_sst
    PUBLIC
        DeltaDB::common
        xxhash::xxhash
    PRIVATE
        Snappy::snappy
)

add_library(DeltaDB::sst ALIAS db_sst)