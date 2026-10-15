if(BUILD_TESTING)
    add_executable(sha256_test
        ${PROJECT_SOURCE_DIR}/tests/sha256_test.cpp
        ${PROJECT_SOURCE_DIR}/src/utils/Sha256.cpp
    )
    target_include_directories(sha256_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
    add_test(NAME sha256_test COMMAND sha256_test)
endif()
