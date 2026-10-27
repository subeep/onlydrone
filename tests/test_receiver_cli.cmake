execute_process(COMMAND "${HOP}" --band 58 --period 0.64 --burst 0.00065 --probability 0.95
  RESULT_VARIABLE status OUTPUT_VARIABLE output)
if(NOT status EQUAL 0 OR NOT output MATCHES "5756500000" OR NOT output MATCHES "0.95")
  message(FATAL_ERROR "hop plan failed: ${output}")
endif()
foreach(option IN ITEMS "--cycles;0.5" "--band;5" "--period;nan" "--dwell;-1" "--unknown;1")
  execute_process(COMMAND "${HOP}" ${option} RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
  if(status EQUAL 0)
    message(FATAL_ERROR "hop accepted invalid option: ${option}")
  endif()
endforeach()
execute_process(COMMAND "${RF}" "${FIXTURE}" --format sc16 --rate 15360000 --noise-dbfs -100
  RESULT_VARIABLE status OUTPUT_VARIABLE output)
if(NOT status EQUAL 0 OR NOT output MATCHES "unknown_rf" OR NOT output MATCHES "identified_drones\":null")
  message(FATAL_ERROR "RF CLI failed or incorrectly claims identified drones: ${output}")
endif()
execute_process(COMMAND "${DF}" "${FIXTURE}" "${FIXTURE}" --cal missing.toml
  RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
if(status EQUAL 0)
  message(FATAL_ERROR "DF accepted unaligned input")
endif()
set(calibration "${CMAKE_CURRENT_BINARY_DIR}/receiver-test-phase-cal.toml")
string(TIMESTAMP now "%s" UTC)
execute_process(COMMAND "${DF}" "${FIXTURE}" "${FIXTURE}" --aligned --splitter-calibrate
  --format sc16 --rate 15360000 --center 2459500000 --samples 16000 --spacing-m 0.05
  --measured-unix-ns "${now}000000000" RESULT_VARIABLE status OUTPUT_FILE "${calibration}")
if(NOT status EQUAL 0)
  message(FATAL_ERROR "DF synthetic same-channel calibration failed")
endif()
execute_process(COMMAND "${DF}" "${FIXTURE}" "${FIXTURE}" --aligned --cal "${calibration}"
  --format sc16 --rate 15360000 --center 2459500000 --samples 8000 --start-sample 8000
  RESULT_VARIABLE status OUTPUT_VARIABLE output)
file(REMOVE "${calibration}")
if(NOT status EQUAL 0 OR NOT output MATCHES "broadside_deg\":0" OR NOT output MATCHES "ambiguous_front_back\":true")
  message(FATAL_ERROR "DF CLI roundtrip failed: ${output}")
endif()
