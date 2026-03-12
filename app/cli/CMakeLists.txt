file(GLOB_RECURSE CLI_SOURCES *.cpp)

add_library(db_cli STATIC ${CLI_SOURCES})

target_include_directories(db_cli
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(db_cli
    PUBLIC
        DeltaDB::kv
        tinyxml::tinyxml
    PRIVATE
        xxhash::xxhash
        Snappy::snappy
        ZLIB::ZLIB
)

add_library(DeltaDB::cli ALIAS db_cli)