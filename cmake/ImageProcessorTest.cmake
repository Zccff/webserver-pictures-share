if(BUILD_TESTING)
    add_executable(image_processor_test
        ${PROJECT_SOURCE_DIR}/tests/image_processor_test.cpp
        ${PROJECT_SOURCE_DIR}/src/image/ImageProcessor.cpp
    )
    target_include_directories(image_processor_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(image_processor_test PRIVATE pthread)

    add_test(NAME image_processor_test COMMAND image_processor_test)

    add_executable(image_processor_timeout_test
        ${PROJECT_SOURCE_DIR}/tests/image_processor_timeout_test.cpp
        ${PROJECT_SOURCE_DIR}/src/image/ImageProcessor.cpp
    )
    target_include_directories(image_processor_timeout_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(image_processor_timeout_test PRIVATE pthread)

    add_test(NAME image_processor_timeout_test COMMAND image_processor_timeout_test)
endif()
