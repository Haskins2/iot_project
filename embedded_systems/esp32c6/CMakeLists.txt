# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Extract git version tag for OTA version checking
execute_process(
    COMMAND git describe --tags --always
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX REPLACE "^v" "" GIT_VERSION "${GIT_VERSION}")
add_compile_definitions(CURRENT_VERSION="${GIT_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# NOTE: MINIMAL_BUILD removed — BLE requires many indirect components
project(esp32c6)
