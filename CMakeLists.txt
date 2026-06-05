cmake_minimum_required(VERSION 3.25)

project(CrimsonDesertAnalogMovement LANGUAGES CXX)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../tkhquang-CrimsonDesertTools/CrimsonDesertCore"
    "${CMAKE_CURRENT_BINARY_DIR}/CrimsonDesertCore"
)

add_library(CrimsonDesertAnalogMovement SHARED
    src/main.cpp
)

target_compile_features(CrimsonDesertAnalogMovement PRIVATE cxx_std_23)
target_compile_definitions(CrimsonDesertAnalogMovement PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(CrimsonDesertAnalogMovement PRIVATE
    CrimsonDesertCore::core
    user32
    xinput
)

set_target_properties(CrimsonDesertAnalogMovement PROPERTIES
    OUTPUT_NAME "CrimsonDesertAnalogMovement"
    SUFFIX ".asi"
)
