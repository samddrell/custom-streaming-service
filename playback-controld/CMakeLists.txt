cmake_minimum_required(VERSION 3.22)
project(playback-controld CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB REQUIRED taglib)

# Everything except main.cpp, shared between the daemon binary and the test binary.
add_library(playback_controld_core
    src/music_library.cpp
    src/control_routes.cpp
    src/stream_route.cpp
)
target_include_directories(playback_controld_core PUBLIC
    src
    third_party
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(playback_controld_core PUBLIC ${TAGLIB_CFLAGS_OTHER})
target_link_libraries(playback_controld_core PUBLIC
    Threads::Threads
    ${TAGLIB_LIBRARIES}
)
target_link_directories(playback_controld_core PUBLIC ${TAGLIB_LIBRARY_DIRS})

add_executable(playback-controld src/main.cpp)
target_link_libraries(playback-controld PRIVATE playback_controld_core)

enable_testing()
add_executable(playback-controld-tests tests/music_library_test.cpp)
target_link_libraries(playback-controld-tests PRIVATE playback_controld_core)
target_compile_definitions(playback-controld-tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME playback-controld-tests COMMAND playback-controld-tests)
