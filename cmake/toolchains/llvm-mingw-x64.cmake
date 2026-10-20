# Cross-compile for Windows x64 with llvm-mingw (clang + lld + mingw-w64, UCRT).
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x64.cmake -DLLVM_MINGW_ROOT=/path/to/llvm-mingw
# On Windows hosts llvm-mingw can also be used natively; this file is for
# Linux/macOS hosts.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
if(NOT LLVM_MINGW_ROOT)
  set(LLVM_MINGW_ROOT $ENV{LLVM_MINGW_ROOT})
endif()
if(NOT LLVM_MINGW_ROOT)
  message(FATAL_ERROR "Set LLVM_MINGW_ROOT to the llvm-mingw installation")
endif()
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)
set(_t x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${LLVM_MINGW_ROOT}/bin/${_t}-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW_ROOT}/bin/${_t}-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW_ROOT}/bin/${_t}-windres)
set(CMAKE_AR ${LLVM_MINGW_ROOT}/bin/llvm-ar)
set(CMAKE_RANLIB ${LLVM_MINGW_ROOT}/bin/llvm-ranlib)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW_ROOT}/${_t})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
