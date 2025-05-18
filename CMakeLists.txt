cmake_minimum_required(VERSION 3.14)
project(AiMatchingEmbedding)

# Set vcpkg toolchain
set(CMAKE_TOOLCHAIN_FILE "C:/Users/preci/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")

set(CMAKE_CXX_STANDARD 17)

# Find SQLite3 through vcpkg
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Configure JSON library
include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(json)

# Directory structure setup
include_directories(include)

# Main executable
add_executable(ai_job_matcher
    src/main.cpp
    src/cv_job_matcher.cpp
)

target_include_directories(ai_job_matcher PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ai_job_matcher PRIVATE 
    unofficial::sqlite3::sqlite3
    nlohmann_json::nlohmann_json
    ${CMAKE_THREAD_LIBS_INIT}
)

# Windows-specific settings
if(WIN32)
    target_compile_definitions(ai_job_matcher PRIVATE NOMINMAX)
    target_link_libraries(ai_job_matcher PRIVATE wsock32 ws2_32)
endif()