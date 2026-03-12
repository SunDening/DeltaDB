enable_testing()


file(GLOB TEST_SOURCES *.cpp)

add_executable(deltadb_test ${TEST_SOURCES})


target_link_libraries(deltadb_test
    PRIVATE
        DeltaDB::common
        DeltaDB::wal
        DeltaDB::sst
        DeltaDB::kv
        GTest::gtest_main
)


include(GoogleTest)
gtest_discover_tests(deltadb_test)