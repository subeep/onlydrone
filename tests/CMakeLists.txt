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
droneid_test(test_receiver)

# Operator UI model (headless; no GPU/window needed).
if(TARGET droneid_ui_model)
  add_executable(test_ui_model test_ui_model.cpp)
  target_link_libraries(test_ui_model PRIVATE droneid_ui_model)
  target_include_directories(test_ui_model PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME test_ui_model COMMAND test_ui_model)
endif()

# ASTM F3411 Open Drone ID: parser/MAC, Wi-Fi PHY on synthetic IQ, CLI (+ UI when built).
if(TARGET droneid_odid)
  add_executable(test_odid test_odid.cpp)
  target_link_libraries(test_odid PRIVATE droneid_odid)
  add_test(NAME test_odid COMMAND test_odid)
  add_executable(test_odid_wifi test_odid_wifi.cpp)
  target_link_libraries(test_odid_wifi PRIVATE droneid_odid)
  target_include_directories(test_odid_wifi PRIVATE ${CMAKE_SOURCE_DIR}/src/odid)
  add_test(NAME test_odid_wifi COMMAND test_odid_wifi)
  set(odid_ui_arg "")
  if(TARGET droneid_ui)
    set(odid_ui_arg -DUI=$<TARGET_FILE:droneid_ui>)
  endif()
  add_test(NAME test_odid_cli COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:odid_gen> -DDECODE=$<TARGET_FILE:odid_decode> ${odid_ui_arg}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/odid_cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_odid_cli.cmake)
endif()

# DJI decoder tests (only when the decoder library was built).
if(TARGET droneid_dji)
  add_test(NAME test_sweep_cli COMMAND ${CMAKE_COMMAND}
    -DSWEEP=$<TARGET_FILE:droneid_sweep> -P ${CMAKE_CURRENT_SOURCE_DIR}/test_sweep_cli.cmake)
  add_executable(test_ui_worker test_ui_worker.cpp)
  target_link_libraries(test_ui_worker PRIVATE droneid_ui_worker)
  target_compile_definitions(test_ui_worker PRIVATE DRONEID_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  droneid_runtime_link(test_ui_worker)
  add_test(NAME test_ui_worker COMMAND test_ui_worker)
  if(TARGET droneid_ui)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME test_df_cli COMMAND ${CMAKE_COMMAND} -E env
        DRONEID_BUILD_DIR=${CMAKE_BINARY_DIR} ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_df_cli.py)
    endif()
    add_test(NAME test_ui_cli COMMAND ${CMAKE_COMMAND}
      -DUI=$<TARGET_FILE:droneid_ui>
      -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/data/mini2_wideband.cf32
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test_ui_cli.cmake)
  endif()
  add_executable(test_dji test_dji.cpp)
  target_link_libraries(test_dji PRIVATE droneid_dji)
  target_compile_definitions(test_dji PRIVATE DRONEID_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  add_test(NAME test_dji COMMAND test_dji)
  add_executable(test_resampler test_resampler.cpp)
  target_link_libraries(test_resampler PRIVATE droneid_dji)
  add_test(NAME test_resampler COMMAND test_resampler)
  add_executable(test_rf_metrics test_rf_metrics.cpp)
  target_link_libraries(test_rf_metrics PRIVATE droneid_rf)
  add_test(NAME test_rf_metrics COMMAND test_rf_metrics)
  add_executable(test_o4_detector test_o4_detector.cpp)
  target_link_libraries(test_o4_detector PRIVATE droneid_rf)
  add_test(NAME test_o4_detector COMMAND test_o4_detector)
  add_test(NAME test_receiver_cli COMMAND ${CMAKE_COMMAND}
    -DHOP=$<TARGET_FILE:droneid_hop>
    -DRF=$<TARGET_FILE:rf_analyze>
    -DDF=$<TARGET_FILE:df_bearing>
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/data/mini2_centered.sc16
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_receiver_cli.cmake)
  add_test(NAME test_decode_cli COMMAND ${CMAKE_COMMAND}
    -DDECODER=$<TARGET_FILE:droneid_decode>
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/data/mini2_wideband.cf32
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_decode_cli.cmake)
endif()
