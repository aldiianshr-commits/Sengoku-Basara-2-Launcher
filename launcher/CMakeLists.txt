# SB2Launcher: library, recompilation pipeline, settings, process control.
set(_res)
if(WIN32)
  set(_res ${PROJECT_SOURCE_DIR}/packaging/windows/app.rc)
endif()
add_library(sb2builder STATIC src/builder.cpp src/library.cpp src/game_session.cpp)
target_include_directories(sb2builder PUBLIC src)
target_link_libraries(sb2builder PUBLIC sb2scanner ps2recomp_lib Threads::Threads)

add_executable(sb2launcher WIN32 src/main.cpp ${_res})
target_link_libraries(sb2launcher PRIVATE sb2ui sb2builder)
set_target_properties(sb2launcher PROPERTIES OUTPUT_NAME SB2Launcher)

add_executable(sb2build src/build_cli.cpp)
target_link_libraries(sb2build PRIVATE sb2builder)

# ----------------------------------------------------------------------------
# SDK used by the launcher to link recompiled games on the user's machine.
#   build tree:  <build>/sdk/sdk.json with absolute paths (development)
#   install:     <prefix>/sdk/{include,lib,sdk.json} + bundled toolchain
if(WIN32)
  add_library(sb2gameres OBJECT ${PROJECT_SOURCE_DIR}/packaging/windows/app.rc)
  set(SB2_SYSLIBS d3d12 dxgi dxguid d3dcompiler setupapi winmm imm32 version ole32 oleaut32 uuid gdi32 user32 shell32 advapi32 cfgmgr32 hid)
  set(SB2_LDFLAGS -static -mwindows -Wl,--stack,8388608)
  set(SB2_EXE_SUFFIX ".exe")
else()
  set(SB2_SYSLIBS pthread dl m)
  set(SB2_LDFLAGS -Wl,--as-needed)
  set(SB2_EXE_SUFFIX "")
endif()
set(SB2_SDK_CXXFLAGS -std=c++20 -fno-strict-aliasing -w)

function(_sb2_json_list out)
  set(items)
  foreach(x IN LISTS ARGN)
    list(APPEND items "\"${x}\"")
  endforeach()
  list(JOIN items ", " joined)
  set(${out} "[${joined}]" PARENT_SCOPE)
endfunction()

set(_sdk_targets sb2game sb2ui imgui ps2runtime sb2common)
set(_dev_libs)
foreach(t IN LISTS _sdk_targets)
  list(APPEND _dev_libs "$<TARGET_FILE:${t}>")
endforeach()
list(APPEND _dev_libs "$<TARGET_FILE:${SB2_SDL3_TARGET}>")
_sb2_json_list(_dev_libs_json ${_dev_libs})
_sb2_json_list(_sys_json ${SB2_SYSLIBS})
_sb2_json_list(_ld_json ${SB2_LDFLAGS})
_sb2_json_list(_cxx_json ${SB2_SDK_CXXFLAGS})
_sb2_json_list(_inc_json ${PROJECT_SOURCE_DIR}/runtime/include ${PROJECT_SOURCE_DIR}/game/include)
set(_dev_objs "[]")
if(WIN32)
  set(_dev_objs "[\"$<TARGET_OBJECTS:sb2gameres>\"]")
endif()
file(GENERATE OUTPUT ${PROJECT_BINARY_DIR}/sdk/sdk.json CONTENT "{
  \"version\": \"${PROJECT_VERSION}\",
  \"cxx\": \"${CMAKE_CXX_COMPILER}\",
  \"include\": ${_inc_json},
  \"cxxflags\": ${_cxx_json},
  \"libs\": ${_dev_libs_json},
  \"syslibs\": ${_sys_json},
  \"ldflags\": ${_ld_json},
  \"objects\": ${_dev_objs},
  \"exe_suffix\": \"${SB2_EXE_SUFFIX}\"
}
")
add_custom_target(sb2sdk ALL DEPENDS ${_sdk_targets})

# Installed layout.
set(_inst_libs)
foreach(t IN LISTS _sdk_targets)
  list(APPEND _inst_libs "lib/lib${t}.a")
endforeach()
list(APPEND _inst_libs "lib/libSDL3.a")
_sb2_json_list(_inst_libs_json ${_inst_libs})
set(_inst_objs "[]")
if(WIN32)
  set(_inst_objs "[\"lib/sb2game_res.o\"]")
  set(_inst_cxx "../toolchain/bin/x86_64-w64-mingw32-clang++.exe")
else()
  set(_inst_cxx "c++")
endif()
file(WRITE ${PROJECT_BINARY_DIR}/sdk-install/sdk.json "{
  \"version\": \"${PROJECT_VERSION}\",
  \"cxx\": \"${_inst_cxx}\",
  \"include\": [\"include\"],
  \"cxxflags\": ${_cxx_json},
  \"libs\": ${_inst_libs_json},
  \"syslibs\": ${_sys_json},
  \"ldflags\": ${_ld_json},
  \"objects\": ${_inst_objs},
  \"exe_suffix\": \"${SB2_EXE_SUFFIX}\"
}
")

install(TARGETS sb2launcher RUNTIME DESTINATION .)
install(TARGETS ps2recomp sb2scan sb2build RUNTIME DESTINATION tools)
install(FILES ${PROJECT_BINARY_DIR}/sdk-install/sdk.json DESTINATION sdk)
foreach(t IN LISTS _sdk_targets)
  install(FILES $<TARGET_FILE:${t}> DESTINATION sdk/lib RENAME lib${t}.a)
endforeach()
install(FILES $<TARGET_FILE:${SB2_SDL3_TARGET}> DESTINATION sdk/lib RENAME libSDL3.a)
if(WIN32)
  install(FILES $<TARGET_OBJECTS:sb2gameres> DESTINATION sdk/lib RENAME sb2game_res.o)
endif()
install(DIRECTORY ${PROJECT_SOURCE_DIR}/runtime/include/ps2 DESTINATION sdk/include)
install(FILES ${PROJECT_SOURCE_DIR}/game/include/sb2/game.hpp DESTINATION sdk/include/sb2)
install(FILES ${PROJECT_SOURCE_DIR}/README.md DESTINATION . OPTIONAL)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs DESTINATION . OPTIONAL)
