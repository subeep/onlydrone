# Phase 0 tests. Uses the bundled header-only microtest harness by default;
# flip DRONEID_WITH_GTEST=ON once system GoogleTest is installed.
function(droneid_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE droneid_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

droneid_test(test_ring_buffer)
droneid_test(test_file_source)
droneid_test(test_config)

# DJI decoder tests (only when the decoder library was built).
if(TARGET droneid_dji)
  add_executable(test_dji test_dji.cpp)
  target_link_libraries(test_dji PRIVATE droneid_dji)
  target_compile_definitions(test_dji PRIVATE DRONEID_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  add_test(NAME test_dji COMMAND test_dji)
endif()
