cmake_minimum_required( VERSION 3.19 )

set( library_name SampleApp )
project( ${library_name} C )
set( CMAKE_INCLUDE_CURRENT_DIR ON )

set( DEFAULT_HEADERS_DIR ${PROJECT_SOURCE_DIR}/include )
link_directories( ${PROJECT_SOURCE_DIR}/lib )

file( GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/src/main.c )

add_executable( ${library_name} ${SRC_FILES} )

if (WIN32 )
target_link_libraries( ${library_name} PRIVATE libvlccore.lib libvlc.lib )
else()
target_link_libraries( ${library_name} PRIVATE libvlccore.so libvlc.so )
endif()

target_include_directories( ${library_name} PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${DEFAULT_HEADERS_DIR} )
