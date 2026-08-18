add_executable(reframe-tests
    test_main.cpp
    test_pacer.cpp
    test_settings.cpp
    test_replay_buffer.cpp
)
rf_target_defaults(reframe-tests)
target_link_libraries(reframe-tests PRIVATE rf::core)
target_include_directories(reframe-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(reframe-tests PROPERTIES FOLDER "tests")

add_test(NAME reframe-tests COMMAND reframe-tests)
