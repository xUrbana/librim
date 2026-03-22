cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER /usr//bin/clang++)

project(librim VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE RelWithDebInfo)

find_package(Boost REQUIRED COMPONENTS system)

include(FetchContent)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        12.1.0
)
FetchContent_MakeAvailable(fmt)

add_library(librim INTERFACE)
target_include_directories(librim INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(librim INTERFACE Boost::system spdlog::spdlog)

# Enable checking for warnings
target_compile_options(librim INTERFACE -Wall -Wextra -Wpedantic)

# Benchmark
add_subdirectory(benchmark)

# Examples
add_subdirectory(examples)

# Tests
enable_testing()
add_subdirectory(tests)
