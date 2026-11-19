# Black (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(black LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git
                         GIT_TAG 91146ff6e2a218b19c1109dc665d06020472ab57)  # newer SDKs change the Backend API
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)
# src/sound, src/d3d, src/audio, src/app/game_code.cpp and src/common/game_files.cpp replace the SDK ones
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_code|app/game_files)\\.cpp$")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
# gyro aiming and the compiler window read the controllers through SDL
set_source_files_properties(src/common/gyro.cpp src/app/game_code.cpp
                            PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")

# partitions as config/game.toml sets them
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

add_xbe_image(Black default.xbe game.toml ${GAME_PARTITIONS} game)

# VR: the OpenXR loader, linked in statically
include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git GIT_TAG release-1.1.49
                     GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openxr)
target_link_libraries(Black PRIVATE openxr_loader)

# build_info.h for the version resource, rewritten only when the commit changes
find_package(Git QUIET)
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO}
    VERBATIM)
# version.rc also embeds src/common/europe.patch
set_source_files_properties(src/common/version.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    OBJECT_DEPENDS "${BUILD_INFO};${CMAKE_CURRENT_SOURCE_DIR}/src/common/europe.patch")
target_sources(Black PRIVATE src/common/version.rc)
add_dependencies(Black build_info)
set_target_properties(Black PROPERTIES OUTPUT_NAME BLACK)

# BLACK_VR.exe: starts BLACK.exe in VR (--vr, vr::Wanted); BLACK.exe alone is the keyboard and mouse game
add_executable(BlackVR WIN32 tools/vr_launcher.cpp)
set_target_properties(BlackVR PROPERTIES OUTPUT_NAME BLACK_VR)
target_compile_definitions(BlackVR PRIVATE UNICODE _UNICODE)
target_link_options(BlackVR PRIVATE -municode)
add_dependencies(BlackVR Black)

# VR: the player's models (tools/vr_assets.py makes them) beside the executable
file(GLOB VR_MODELS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vr_assets/*.bvr)
add_custom_command(TARGET Black POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:Black>/vr_assets
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VR_MODELS} $<TARGET_FILE_DIR:Black>/vr_assets
    VERBATIM)
