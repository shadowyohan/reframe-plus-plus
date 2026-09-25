add_executable(reframe-tests
    test_main.cpp
    test_pacer.cpp
    test_settings.cpp
    test_replay_buffer.cpp
    test_frame_bridge.cpp
    test_lang.cpp
    test_text_fits.cpp
    test_texture_cache.cpp
    test_noise_suppressor.cpp
    test_app_audio_tracks.cpp
    test_track_names.cpp
    test_cursor_painter.cpp
    test_integrations.cpp
    test_clip_cropper.cpp
    test_update_check.cpp
    ${CMAKE_SOURCE_DIR}/third_party/rnnoise/rnnoise_model.rc
)
rf_target_defaults(reframe-tests)
target_link_libraries(reframe-tests PRIVATE rf::core)
target_include_directories(reframe-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(reframe-tests PROPERTIES FOLDER "tests")

add_test(NAME reframe-tests COMMAND reframe-tests)
