if(BUILD_TESTING)
    add_executable(redis_metrics_test
        ${PROJECT_SOURCE_DIR}/tests/redis_metrics_test.cpp
        ${PROJECT_SOURCE_DIR}/src/utils/Metrics.cpp
    )
    target_include_directories(redis_metrics_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )

    add_test(NAME redis_metrics_test COMMAND redis_metrics_test)
endif()
