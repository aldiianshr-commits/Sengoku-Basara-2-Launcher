# Third-party dependencies for the front-end and launcher: SDL3 and Dear ImGui.
# Local checkouts can be supplied with -DSB2_SDL3_DIR=<install prefix> or
# -DFETCHCONTENT_SOURCE_DIR_IMGUI=<source dir> to build offline.
include(FetchContent)

set(SB2_SDL3_TAG release-3.4.8)
set(SB2_IMGUI_TAG v1.92.9)

if(SB2_SDL3_DIR)
  list(PREPEND CMAKE_PREFIX_PATH ${SB2_SDL3_DIR})
endif()
find_package(SDL3 CONFIG QUIET COMPONENTS SDL3-static)
if(TARGET SDL3::SDL3-static)
  message(STATUS "Using SDL3 from ${SDL3_DIR}")
  set(SB2_SDL3_TARGET SDL3::SDL3-static)
else()
  message(STATUS "Fetching SDL3 ${SB2_SDL3_TAG}")
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG ${SB2_SDL3_TAG} GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3)
  set(SB2_SDL3_TARGET SDL3::SDL3-static)
endif()

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG ${SB2_IMGUI_TAG} GIT_SHALLOW TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()

set(_imgui_src
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
if(WIN32)
  list(APPEND _imgui_src ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
endif()
add_library(imgui STATIC ${_imgui_src})
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC ${SB2_SDL3_TARGET})
if(WIN32)
  target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler dxguid)
endif()
if(NOT MSVC)
  target_compile_options(imgui PRIVATE -w)
endif()
