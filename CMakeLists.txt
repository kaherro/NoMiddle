cmake_minimum_required(VERSION 3.16)
project(NoMiddle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)

include(FetchContent)

FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.5
)
FetchContent_MakeAvailable(cpr)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(SQLite3 REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include CACHE PATH "Asio include dir" FORCE)

FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Crow)

add_executable(server
    src/server/main.cpp
    src/db/db_manager.cpp
    src/server/auth.cpp
    src/server/http_client.cpp
    src/server/message_delivery.cpp
    src/server/retry_worker.cpp
    src/server/crypto/key_manager.cpp
    src/server/crypto/message_crypto.cpp
)

target_include_directories(server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIBSODIUM_INCLUDE_DIRS}
)

target_link_libraries(server PRIVATE
    Crow::Crow
    SQLite::SQLite3
    Threads::Threads
    cpr::cpr
    ${LIBSODIUM_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_features(server PRIVATE cxx_std_20)