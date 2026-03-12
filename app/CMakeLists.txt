add_subdirectory(cli)

add_executable(deltadb main.cpp)

target_link_libraries(deltadb
    PRIVATE
        DeltaDB::cli
        DeltaDB::kv
)