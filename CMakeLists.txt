cmake_minimum_required(VERSION 3.21)
project(PressFtoPayRespects VERSION 1.4.0 LANGUAGES CXX)

# Deploy target — mod FOLDER gets --Claude; DLL name stays plain.
set(OUTPUT_FOLDER "X:/MODDINGSSE/modorganizer2/mods/Press F To Pay Respects--Claude")

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    src/main.cpp
    src/Settings.cpp
    src/PromptManager.cpp
    src/RespectManager.cpp
    src/PickupManager.cpp
    src/MsvcShim.cpp
)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

# SimpleIni is vendored header-only in include/.
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "SKSE plugin output: ${DLL_FOLDER}")

add_custom_command(
    TARGET "${PROJECT_NAME}" POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    VERBATIM
)
