if(BUILD_TESTING)
    add_executable(image_deletion_reconciler_test
        tests/image_deletion_reconciler_test.cpp
        src/image/ImageDeletionReconciler.cpp
    )
    target_include_directories(image_deletion_reconciler_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    add_test(
        NAME image_deletion_reconciler_test
        COMMAND image_deletion_reconciler_test
    )
endif()
