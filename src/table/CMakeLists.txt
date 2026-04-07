file(GLOB TABLE_SOURCES *.cpp *.cc)

if(TABLE_SOURCES)
    add_library(table STATIC ${TABLE_SOURCES})
    
    target_include_directories(table
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/db/include
    )
    
    target_link_libraries(table
        PUBLIC
            DeltaDB::utils
            DeltaDB::wal
    )
    
    add_library(DeltaDB::table ALIAS table)
else()
    add_library(table INTERFACE)
    target_include_directories(table
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_library(DeltaDB::table ALIAS table)
endif()