# Unit tests (no external framework).
add_executable(sb2_unit_tests
  unit/main.cpp)
target_link_libraries(sb2_unit_tests PRIVATE ps2runtime ps2recomp_lib)
target_include_directories(sb2_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src)
add_test(NAME unit COMMAND sb2_unit_tests)
