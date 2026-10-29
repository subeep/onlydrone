execute_process(COMMAND "${SWEEP}" --profile balanced --jitter 0
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT output MATCHES "\"steps\":33" OR
   NOT output MATCHES "28.430000" OR NOT output MATCHES "2478500000" OR
   NOT output MATCHES "5845000000" OR NOT output MATCHES "wifi_remote_id")
  message(FATAL_ERROR "invalid sweep plan: ${output} ${error}")
endif()
execute_process(COMMAND "${SWEEP}" --profile thorough --include-52
  RESULT_VARIABLE status OUTPUT_VARIABLE output)
if(NOT status EQUAL 0 OR NOT output MATCHES "\"steps\":51" OR NOT output MATCHES "5175000000")
  message(FATAL_ERROR "invalid expanded sweep: ${output}")
endif()
foreach(option IN ITEMS "--profile;bad" "--cycles;0.5" "--settle;0" "--jitter;nan" "--gain;-1")
  execute_process(COMMAND "${SWEEP}" ${option} RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
  if(status EQUAL 0)
    message(FATAL_ERROR "accepted invalid sweep option ${option}")
  endif()
endforeach()
