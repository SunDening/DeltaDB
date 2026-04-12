file(GLOB TEST_SOURCES *.cpp)

add_executable(my_test ${TEST_SOURCES})

target_link_libraries(my_test
    PRIVATE
        GTest::gtest_main
        GTest::gtest

        DeltaDB::db
        DeltaDB::wal
        DeltaDB::utils

        tinyxml::tinyxml
        crc32c::crc32c
)

include(GoogleTest)
gtest_discover_tests(my_test)