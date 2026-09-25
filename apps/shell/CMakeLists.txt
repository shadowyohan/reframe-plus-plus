add_executable(reframe WIN32 main.cpp app.rc
    ${CMAKE_SOURCE_DIR}/third_party/rnnoise/rnnoise_model.rc)
rf_target_defaults(reframe)
target_compile_definitions(reframe PRIVATE
    RF_VERSION="${PROJECT_VERSION}"
    RF_VERSION_SHORT="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    RF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    RF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    RF_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    RF_VERSION_BUILD=${RF_BUILD_NUMBER}
    RF_VERSION_STR="${PROJECT_VERSION}.${RF_BUILD_NUMBER}")
target_link_libraries(reframe PRIVATE rf::core shell32 user32 dbghelp)
set_target_properties(reframe PROPERTIES FOLDER "apps" OUTPUT_NAME "Reframe")

target_sources(reframe PRIVATE app.manifest)
