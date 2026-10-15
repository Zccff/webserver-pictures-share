if(BUILD_TESTING)
    add_executable(streaming_upload_test
        ${PROJECT_SOURCE_DIR}/tests/streaming_upload_test.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpConnection.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpConnectionUpload.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRequestUpload.cpp
        ${PROJECT_SOURCE_DIR}/src/log/AsyncLogger.cpp
        ${PROJECT_SOURCE_DIR}/src/utils/Metrics.cpp
    )
    target_include_directories(streaming_upload_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(streaming_upload_test PRIVATE pthread)
    add_test(NAME streaming_upload_test COMMAND streaming_upload_test)
endif()
