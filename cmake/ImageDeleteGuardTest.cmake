if(BUILD_TESTING)
    add_executable(image_delete_guard_test
        tests/image_delete_guard_test.cpp
        src/image/ImageDeleteGuard.cpp
    )
    target_include_directories(image_delete_guard_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    add_test(NAME image_delete_guard_test COMMAND image_delete_guard_test)
endif()
