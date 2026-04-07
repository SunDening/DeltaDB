file(GLOB TEST_SOURCES *.cpp)

add_executable(my_test ${TEST_SOURCES})  # ????? test_log.cpp

target_include_directories(my_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_SOURCE_DIR}/src/utils/include
        ${CMAKE_SOURCE_DIR}/src/wal/include
        ${CMAKE_SOURCE_DIR}/src/table/include
        ${CMAKE_SOURCE_DIR}/src/db/include
)

target_link_libraries(my_test
    PRIVATE
        # 1. ?????????????????
        GTest::gtest_main
        GTest::gtest
        
        # 2. ???????????? ? ???
        DeltaDB::db        # ??????????
        DeltaDB::wal       # ???
        DeltaDB::utils     # ??
        
        # 3. ????????????
        # spdlog::spdlog
        tinyxml::tinyxml         # ????
        crc32c::crc32c  # ÃÌº”crc32c¡¥Ω”
)

include(GoogleTest)
gtest_discover_tests(my_test)