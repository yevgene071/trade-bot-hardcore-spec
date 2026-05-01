cmake_minimum_required(VERSION 3.28)
project(TradeBotHardcore 
    VERSION 0.1.0 
    LANGUAGES CXX)

# Force C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_AVX2 "Enable AVX2 optimizations" ON)

# Cross-platform compiler flags
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8)
    if(ENABLE_AVX2)
        add_compile_options(/arch:AVX2)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    if(ENABLE_AVX2)
        add_compile_options(-mavx2)
    endif()
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Include directories
include_directories(include)
include_directories(src)

# Add source files
file(GLOB_RECURSE SRC_FILES 
    src/*.cpp 
    src/*.hpp 
    include/*.hpp)

# Main executable
add_executable(${PROJECT_NAME} src/main.cpp ${SRC_FILES})

# Link libraries (Placeholder for future dependencies)
# target_link_libraries(${PROJECT_NAME} PRIVATE ...)

if(BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)
endif()

message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "AVX2: ${ENABLE_AVX2}")
