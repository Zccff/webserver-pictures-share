if(BUILD_TESTING)
    add_executable(http_image_cache_invalidation_test
        ${PROJECT_SOURCE_DIR}/tests/http_image_cache_invalidation_test.cpp
        ${PROJECT_SOURCE_DIR}/src/auth/AsyncSessionAdapter.cpp
        ${PROJECT_SOURCE_DIR}/src/cache/ImageRedisService.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpResponseUploadFile.cpp
        ${PROJECT_SOURCE_DIR}/src/cache/ImageRedisRanking.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpResponseRanking.cpp
        ${PROJECT_SOURCE_DIR}/src/utils/Sha256.cpp
        ${PROJECT_SOURCE_DIR}/src/cache/LRUCache.cpp
        ${PROJECT_SOURCE_DIR}/src/db/UserStore.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpResponse.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRequestUpload.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpResponseRedis.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpResponseImageStatus.cpp
        ${PROJECT_SOURCE_DIR}/src/http/HttpRedisPolicy.cpp
        ${PROJECT_SOURCE_DIR}/src/image/ImageActivationJournal.cpp
        ${PROJECT_SOURCE_DIR}/src/image/ImageDeleteGuard.cpp
        ${PROJECT_SOURCE_DIR}/src/image/ImageProcessor.cpp
        ${PROJECT_SOURCE_DIR}/src/log/AsyncLogger.cpp
        ${PROJECT_SOURCE_DIR}/src/redis/AsyncRedisClient.cpp
        ${PROJECT_SOURCE_DIR}/src/utils/Metrics.cpp
    )

    target_include_directories(http_image_cache_invalidation_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${HIREDIS_INCLUDE_DIRS}
        ${LIBUV_INCLUDE_DIRS}
    )

    target_compile_options(http_image_cache_invalidation_test PRIVATE
        ${HIREDIS_CFLAGS_OTHER}
        ${LIBUV_CFLAGS_OTHER}
    )

    target_link_libraries(http_image_cache_invalidation_test PRIVATE
        pthread
        ${HIREDIS_LDFLAGS}
        ${LIBUV_LDFLAGS}
    )

    add_test(NAME http_image_cache_invalidation_test
             COMMAND http_image_cache_invalidation_test)
    set_tests_properties(http_image_cache_invalidation_test PROPERTIES
        SKIP_RETURN_CODE 77
    )
endif()
