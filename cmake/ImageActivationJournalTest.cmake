if(BUILD_TESTING)
    add_executable(image_activation_journal_test
        tests/image_activation_journal_test.cpp
        src/image/ImageActivationJournal.cpp
    )
    target_include_directories(image_activation_journal_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    add_test(NAME image_activation_journal_test COMMAND image_activation_journal_test)
endif()
