cmake_minimum_required(VERSION 3.22)
project(cli-lib-controld CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)

# Everything except main.cpp, shared between the gj binary and the test binary.
add_library(gj_core
    src/http_client.cpp
    src/commands.cpp
    src/output.cpp
    src/listen_command.cpp
)
target_include_directories(gj_core PUBLIC
    src
    third_party
)
target_link_libraries(gj_core PUBLIC Threads::Threads)
if(WIN32)
  # cpp-httplib needs Winsock for sockets on Windows (design.md §1) -- playback-controld doesn't
  # need this since it only ever builds on Linux.
  target_link_libraries(gj_core PUBLIC ws2_32)
endif()

add_executable(gj src/main.cpp)
target_link_libraries(gj PRIVATE gj_core)
if(WIN32)
  # CommandLineToArgvW (main.cpp's UTF-8 argv reconstruction) lives in shell32.
  target_link_libraries(gj PRIVATE shell32)
endif()

enable_testing()
add_executable(gj-tests tests/output_test.cpp tests/commands_test.cpp)
target_link_libraries(gj-tests PRIVATE gj_core)
add_test(NAME gj-tests COMMAND gj-tests)
