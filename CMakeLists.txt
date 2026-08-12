cmake_minimum_required(VERSION 3.10)

project(MyWebServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${PROJECT_SOURCE_DIR}/include)

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(MYSQLCLIENT QUIET mysqlclient)
endif()

file(GLOB_RECURSE SRC_FILES
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)

add_executable(my_web_server
    ${SRC_FILES}
)

target_link_libraries(my_web_server pthread)

if(MYSQLCLIENT_FOUND)
    target_compile_definitions(my_web_server PRIVATE MYWEBSERVER_WITH_MYSQL)
    target_include_directories(my_web_server PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
    target_compile_options(my_web_server PRIVATE ${MYSQLCLIENT_CFLAGS_OTHER})
    target_link_libraries(my_web_server ${MYSQLCLIENT_LDFLAGS})
    message(STATUS "MySQL support enabled")
else()
    message(WARNING "mysqlclient was not found. Register/login APIs will report database unavailable.")
endif()
