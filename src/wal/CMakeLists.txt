file(GLOB WAL_SOURCES *.cpp *.cc)

if(WAL_SOURCES)
    add_library(wal STATIC ${WAL_SOURCES})
    
    target_include_directories(wal
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    
    target_link_libraries(wal
        PUBLIC
            DeltaDB::utils
    )
    
    add_library(DeltaDB::wal ALIAS wal)
else()
    add_library(wal INTERFACE)
    target_include_directories(wal
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_library(DeltaDB::wal ALIAS wal)
endif()