cmake_minimum_required(VERSION 3.16)
project(tcp_interview_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(tcp_interview_lib
    src/protocol.cpp
    src/server.cpp
)
target_include_directories(tcp_interview_lib PUBLIC include)
target_link_libraries(tcp_interview_lib PUBLIC Threads::Threads)

add_executable(tcp_server src/main.cpp)
target_link_libraries(tcp_server PRIVATE tcp_interview_lib)

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol_tests tests/protocol_tests.cpp)
    target_link_libraries(protocol_tests PRIVATE tcp_interview_lib)
    add_test(NAME protocol_tests COMMAND protocol_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME server_pool_tests COMMAND ${CMAKE_COMMAND} -E env
        "SERVER_BIN=$<TARGET_FILE:tcp_server>"
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_pool_tests.py)
    set_tests_properties(server_pool_tests PROPERTIES TIMEOUT 60)
endif()
