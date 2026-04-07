file(GLOB DB_SOURCES *.cpp *.cc)

if(DB_SOURCES)
    add_library(db STATIC ${DB_SOURCES})
    
    target_include_directories(db
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/include
    )
    
    target_link_libraries(db
        PUBLIC
            DeltaDB::utils
            DeltaDB::wal
            DeltaDB::table
    )
    
    add_library(DeltaDB::db ALIAS db)
else()
    add_library(db INTERFACE)
    target_include_directories(db
        INTERFACE
            ${CMAKE_SOURCE_DIR}/src/include
    )
    add_library(DeltaDB::db ALIAS db)
endif()