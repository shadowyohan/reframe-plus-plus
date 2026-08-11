add_executable(reframe-cli main.cpp)
rf_target_defaults(reframe-cli)
target_link_libraries(reframe-cli PRIVATE rf::core)
set_target_properties(reframe-cli PROPERTIES FOLDER "apps" OUTPUT_NAME "reframe-cli")
