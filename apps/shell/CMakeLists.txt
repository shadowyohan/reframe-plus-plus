add_executable(reframe WIN32 main.cpp app.rc)
rf_target_defaults(reframe)
target_link_libraries(reframe PRIVATE rf::core shell32 user32)
set_target_properties(reframe PROPERTIES FOLDER "apps" OUTPUT_NAME "Reframe")

target_sources(reframe PRIVATE app.manifest)
