if(BUILD_TESTING)
    add_executable(et_http_connection_test
        ${PROJECT_SOURCE_DIR}/tests/et_http_connection_test.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpConnection.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpConnectionUpload.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRequestUpload.cpp
        ${PROJECT_SOURCE_DIR}/src/log/AsyncLogger.cpp
        ${PROJECT_SOURCE_DIR}/src/utils/Metrics.cpp
    )
    target_include_directories(et_http_connection_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(et_http_connection_test PRIVATE pthread)

    add_test(NAME et_http_connection_test COMMAND et_http_connection_test)
endif()
