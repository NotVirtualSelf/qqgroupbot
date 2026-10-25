cmake_minimum_required(VERSION 3.20)
project(qq_bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-g)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(glaze REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

add_executable(qq_bot
    src/main.cpp
    src/core/ws_server.cpp
    src/protocol/rpc_client.cpp
    src/modules/music/database.cpp
    src/modules/music/netease_client.cpp
    src/modules/music/qqmusic_client.cpp
    src/modules/music/daily_music_manager.cpp
    src/modules/music/music_module.cpp
)

target_include_directories(qq_bot
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(qq_bot
    PRIVATE
        Boost::boost
        OpenSSL::SSL
        OpenSSL::Crypto
        glaze::glaze
        SQLite::SQLite3
        CURL::libcurl
)

add_executable(test_music
    tests/test_music.cpp
    src/modules/music/database.cpp
    src/modules/music/netease_client.cpp
    src/modules/music/qqmusic_client.cpp
    src/modules/music/daily_music_manager.cpp
    src/modules/music/music_module.cpp
    src/protocol/rpc_client.cpp
)

target_include_directories(test_music
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(test_music
    PRIVATE
        Boost::boost
        OpenSSL::SSL
        OpenSSL::Crypto
        glaze::glaze
        SQLite::SQLite3
        CURL::libcurl
)

