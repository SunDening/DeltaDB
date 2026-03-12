file(GLOB_RECURSE KV_SOURCES *.cpp)

add_library(db_kv STATIC ${KV_SOURCES})

target_include_directories(db_kv
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(db_kv
    PUBLIC
        DeltaDB::common
        DeltaDB::wal      # kv ?? wal????
        DeltaDB::sst      # kv ?? sst????
        tinyxml::tinyxml
        xxhash::xxhash
    PRIVATE
        Snappy::snappy
        ZLIB::ZLIB
)

add_library(DeltaDB::kv ALIAS db_kv)