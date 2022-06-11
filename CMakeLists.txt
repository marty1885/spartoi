cmake_minimum_required(VERSION 3.15)
project(spartoi)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
find_package(Drogon REQUIRED)

add_library(spartoi STATIC)
target_sources(spartoi PRIVATE spartoi/SpartanClient.cpp
	spartoi/SpartanServer.cpp
	spartoi/SpartanServerPlugin.cpp)
target_include_directories(spartoi PUBLIC .)
target_link_libraries(spartoi PUBLIC Drogon::Drogon)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/spartoi
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/spartoi
    DESTINATION include
    FILES_MATCHING PATTERN "*.h")
install(TARGETS spartoi
	LIBRARY DESTINATION lib)

option(SPARTOI_BUILD_EXAMPLES "Build Spartoi examples" ON)
if(SPARTOI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# option(SPARTOI_BUILD_TEST "Build Spartoi tests" ON)
# if(SPARTOI_BUILD_TEST)
#     add_subdirectory(tests)
# endif()

