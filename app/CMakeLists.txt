add_executable(deltadb main.cpp)

target_include_directories(deltadb
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_SOURCE_DIR}/src/db/include
        ${CMAKE_SOURCE_DIR}/src/table/include
)

target_link_libraries(deltadb
    PRIVATE
        DeltaDB::db
        DeltaDB::table
        DeltaDB::wal
        DeltaDB::utils
        crc32c
        ZLIB::ZLIB
)
