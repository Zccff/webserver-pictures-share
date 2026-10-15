cmake_minimum_required(VERSION 3.10)

project(MyWebServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include_directories(${PROJECT_SOURCE_DIR}/include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT QUIET mysqlclient)
pkg_check_modules(HIREDIS REQUIRED hiredis)
pkg_check_modules(LIBUV REQUIRED libuv)

file(GLOB_RECURSE SRC_FILES
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)

# The active session path is AsyncSessionManager. Keep the former synchronous
# implementation only as a comparison while excluding it from the server.
list(FILTER SRC_FILES EXCLUDE REGEX ".*/src/auth/SessionManager\\.cpp$")

list(FILTER SRC_FILES EXCLUDE REGEX ".*/src/auth/AsyncSessionManager\\.cpp$")
add_executable(my_web_server
    ${SRC_FILES}
)

target_include_directories(my_web_server PRIVATE
    ${HIREDIS_INCLUDE_DIRS}
    ${LIBUV_INCLUDE_DIRS}
)
target_compile_options(my_web_server PRIVATE
    ${HIREDIS_CFLAGS_OTHER}
    ${LIBUV_CFLAGS_OTHER}
)
target_link_libraries(my_web_server
    pthread
    ${HIREDIS_LDFLAGS}
    ${LIBUV_LDFLAGS}
)

if(MYSQLCLIENT_FOUND)
    target_compile_definitions(my_web_server PRIVATE MYWEBSERVER_WITH_MYSQL)
    target_include_directories(my_web_server PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
    target_compile_options(my_web_server PRIVATE ${MYSQLCLIENT_CFLAGS_OTHER})
    target_link_libraries(my_web_server ${MYSQLCLIENT_LDFLAGS})
    message(STATUS "MySQL support enabled")
else()
    message(WARNING "mysqlclient was not found. Register/login APIs will report database unavailable.")
endif()

include(CTest)
include(cmake/HttpImageRedisIntegrationTest.cmake)
include(cmake/HttpImageCacheInvalidationTest.cmake)
include(cmake/RedisMetricsTest.cmake)
include(cmake/EtHttpConnectionTest.cmake)
include(cmake/ImageProcessorTest.cmake)
include(cmake/HttpThumbnailTest.cmake)
include(cmake/StreamingUploadTest.cmake)
include(cmake/Sha256Test.cmake)
include(cmake/ImageDeleteGuardTest.cmake)
include(cmake/ImageDeletionReconcilerTest.cmake)
include(cmake/ImageActivationJournalTest.cmake)

if(BUILD_TESTING)
    add_executable(async_redis_client_test
        tests/async_redis_client_test.cpp
        src/redis/AsyncRedisClient.cpp
        src/utils/Metrics.cpp
    )
    target_include_directories(async_redis_client_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${HIREDIS_INCLUDE_DIRS}
        ${LIBUV_INCLUDE_DIRS}
    )
    target_compile_options(async_redis_client_test PRIVATE
        ${HIREDIS_CFLAGS_OTHER}
        ${LIBUV_CFLAGS_OTHER}
    )
    target_link_libraries(async_redis_client_test
        pthread
        ${HIREDIS_LDFLAGS}
        ${LIBUV_LDFLAGS}
    )
    add_test(NAME async_redis_client_test COMMAND async_redis_client_test)
    set_tests_properties(async_redis_client_test PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(image_redis_service_test
        tests/image_redis_service_test.cpp
        src/cache/ImageRedisService.cpp
        src/cache/ImageRedisRanking.cpp
        src/redis/AsyncRedisClient.cpp
        src/utils/Metrics.cpp
    )
    target_include_directories(image_redis_service_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${HIREDIS_INCLUDE_DIRS}
        ${LIBUV_INCLUDE_DIRS}
    )
    target_compile_options(image_redis_service_test PRIVATE
        ${HIREDIS_CFLAGS_OTHER}
        ${LIBUV_CFLAGS_OTHER}
    )
    target_link_libraries(image_redis_service_test
        pthread
        ${HIREDIS_LDFLAGS}
        ${LIBUV_LDFLAGS}
    )
    add_test(NAME image_redis_service_test COMMAND image_redis_service_test)
    set_tests_properties(image_redis_service_test PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(http_redis_policy_test
        tests/http_redis_policy_test.cpp
        src/http/HttpRedisPolicy.cpp
    )
    target_include_directories(http_redis_policy_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
    add_test(NAME http_redis_policy_test COMMAND http_redis_policy_test)

    add_executable(thread_pool_capacity_test
        tests/thread_pool_capacity_test.cpp
    )
    target_link_libraries(thread_pool_capacity_test pthread)
    add_test(NAME thread_pool_capacity_test COMMAND thread_pool_capacity_test)

    add_executable(heap_timer_test
        tests/heap_timer_test.cpp
        src/timer/HeapTimer.cpp
    )
    target_link_libraries(heap_timer_test pthread)
    add_test(NAME heap_timer_test COMMAND heap_timer_test)

    add_executable(async_session_manager_test
        tests/async_session_manager_test.cpp
        src/auth/AsyncSessionAdapter.cpp
        src/redis/AsyncRedisClient.cpp
        src/utils/Metrics.cpp
    )
    target_include_directories(async_session_manager_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${HIREDIS_INCLUDE_DIRS}
        ${LIBUV_INCLUDE_DIRS}
    )
    target_compile_options(async_session_manager_test PRIVATE
        ${HIREDIS_CFLAGS_OTHER}
        ${LIBUV_CFLAGS_OTHER}
    )
    target_link_libraries(async_session_manager_test
        pthread
        ${HIREDIS_LDFLAGS}
        ${LIBUV_LDFLAGS}
    )
    add_test(NAME async_session_manager_test COMMAND async_session_manager_test)
    set_tests_properties(async_session_manager_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
