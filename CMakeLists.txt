cmake_minimum_required(VERSION 3.14)
project(AiMatchingEmbedding)

# Set vcpkg toolchain
set(CMAKE_TOOLCHAIN_FILE "C:/Users/preci/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")

set(CMAKE_CXX_STANDARD 17)

# Find required packages
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
# ✅ FIXED: use unofficial-gumbo instead of GumboParser
find_package(unofficial-gumbo CONFIG REQUIRED)

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

# Job Scraper executable
add_executable(job_scraper
    src/scrapper.cpp
)

target_include_directories(job_scraper PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# ✅ FIXED: use unofficial::gumbo::gumbo for linking
target_link_libraries(job_scraper PRIVATE
    CURL::libcurl
    unofficial::gumbo::gumbo
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    ${CMAKE_THREAD_LIBS_INIT}
)

# Define ENABLE_SQLITE to enable SQLite support in the scraper
target_compile_definitions(job_scraper PRIVATE ENABLE_SQLITE)

# Windows-specific settings
if(WIN32)
    target_compile_definitions(ai_job_matcher PRIVATE NOMINMAX)
    target_link_libraries(ai_job_matcher PRIVATE wsock32 ws2_32)
    
    target_compile_definitions(job_scraper PRIVATE NOMINMAX)
    target_link_libraries(job_scraper PRIVATE wsock32 ws2_32)
endif()
