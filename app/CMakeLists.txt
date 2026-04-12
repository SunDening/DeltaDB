add_executable(deltadb main.cpp)

target_link_libraries(deltadb
    PRIVATE
        DeltaDB::db
        DeltaDB::table
        DeltaDB::wal
        DeltaDB::utils
        crc32c
        ZLIB::ZLIB
)
