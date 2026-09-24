add_executable(reframe WIN32 main.cpp app.rc
    ${CMAKE_SOURCE_DIR}/third_party/rnnoise/rnnoise_model.rc)
rf_target_defaults(reframe)
target_link_libraries(reframe PRIVATE rf::core shell32 user32 dbghelp)
set_target_properties(reframe PROPERTIES FOLDER "apps" OUTPUT_NAME "Reframe")

target_sources(reframe PRIVATE app.manifest)
