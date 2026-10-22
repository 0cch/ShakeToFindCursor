cmake_minimum_required(VERSION 3.15)

project(ShakeToFindCursor 
        VERSION 1.4.0 
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

if(NOT WIN32)
    message(FATAL_ERROR "This project only supports Windows platform")
endif()

add_definitions(-DUNICODE -D_UNICODE)

add_executable(${PROJECT_NAME} 
    WIN32 
    main.cpp
    res.rc
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} 
        PRIVATE 
        /W4
        /WX
        /MP
        /EHsc
        /utf-8
    )
    set_target_properties(${PROJECT_NAME}  PROPERTIES
        LINK_FLAGS "/MANIFEST /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\" "
    )
else()
    target_compile_options(${PROJECT_NAME} 
        PRIVATE 
        -Wall 
        -Wextra 
        -Wpedantic
    )
endif()
