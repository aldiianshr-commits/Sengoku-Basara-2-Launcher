cmake_minimum_required(VERSION 3.20)
project(SengokuBasara2HeroesRecompiled VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SB2_BUILD_FRONTEND "Build the SDL3 front-end and Windows launcher" ON)
option(SB2_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- common
add_library(sb2common STATIC
  common/src/hash.cpp
  common/src/elf_loader.cpp
  common/src/disc.cpp)
target_include_directories(sb2common PUBLIC common/include)

# ---------------------------------------------------------------- recompiler
add_library(ps2recomp_lib STATIC
  recompiler/src/r5900_isa.cpp
  recompiler/src/elf.cpp
  recompiler/src/analysis.cpp
  recompiler/src/codegen.cpp)
target_include_directories(ps2recomp_lib PUBLIC recompiler/src)
target_link_libraries(ps2recomp_lib PUBLIC sb2common)

add_executable(ps2recomp recompiler/src/main.cpp)
target_link_libraries(ps2recomp PRIVATE ps2recomp_lib)

# ---------------------------------------------------------------- runtime
add_library(ps2runtime STATIC
  runtime/src/log.cpp
  runtime/src/fiber.cpp
  runtime/src/machine.cpp
  runtime/src/kernel.cpp
  runtime/src/sif.cpp
  runtime/src/iop.cpp
  runtime/src/iop_vfs.cpp
  runtime/src/iop_services.cpp
  runtime/src/hw.cpp
  runtime/src/vu.cpp
  runtime/src/vif.cpp
  runtime/src/gs.cpp
  runtime/src/gs_raster.cpp
  runtime/src/spu2.cpp
  runtime/src/pad.cpp
  runtime/src/cdvd.cpp
  runtime/src/rt.cpp
  runtime/src/host_main.cpp)
target_include_directories(ps2runtime PUBLIC runtime/include PRIVATE runtime/src)
target_link_libraries(ps2runtime PUBLIC sb2common)
find_package(Threads REQUIRED)
target_link_libraries(ps2runtime PUBLIC Threads::Threads)

add_library(ps2main STATIC runtime/src/module_main.cpp)
target_link_libraries(ps2main PUBLIC ps2runtime)

# Compile flags used for generated code (kept in one place so the launcher's
# module builder can use the same ones).
set(SB2_GENERATED_CXX_FLAGS -O2 -fno-strict-aliasing CACHE STRING "Flags for recompiled code")

# ps2_add_recompiled(<target> <elf>): recompile at configure time and build.
function(ps2_add_recompiled target elf)
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen/${target})
  if(NOT EXISTS ${gen}/recomp_table.cpp OR ${elf} IS_NEWER_THAN ${gen}/recomp_table.cpp)
    message(STATUS "Recompiling ${elf}")
    execute_process(COMMAND ${SB2_HOST_RECOMP} --elf ${elf} --out ${gen} --name ${target}
                    RESULT_VARIABLE rc OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "ps2recomp failed for ${elf}")
    endif()
  endif()
  file(GLOB srcs ${gen}/*.cpp)
  add_executable(${target} ${srcs})
  target_compile_options(${target} PRIVATE ${SB2_GENERATED_CXX_FLAGS} -w)
  target_link_libraries(${target} PRIVATE ps2main)
endfunction()

if(SB2_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
