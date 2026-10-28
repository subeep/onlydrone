# Open Drone ID CLI / UI smoke test on SYNTHETIC fixtures written by odid_gen.
# Inputs: -DGEN=odid_gen -DDECODE=odid_decode [-DUI=droneid_ui] -DWORK=<scratch dir>
file(MAKE_DIRECTORY "${WORK}")

function(run expect_rc expected)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  if(NOT rc EQUAL expect_rc OR NOT out MATCHES "${expected}")
    message(FATAL_ERROR "Unexpected result ${rc} (wanted ${expect_rc}) for ${ARGN}:\n${out}\n${err}")
  endif()
endfunction()

function(run_fails)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
  if(rc EQUAL 0)
    message(FATAL_ERROR "Invalid input accepted: ${ARGN}\n${out}")
  endif()
endfunction()

set(beacon "${WORK}/odid_beacon_ofdm.cf32")
set(nan "${WORK}/odid_nan_dsss.sc16")
run(0 "wrote" "${GEN}" "${beacon}" --phy ofdm --mbps 6 --transport beacon --count 3)
run(0 "wrote" "${GEN}" "${nan}" --phy dsss --mbps 2 --short-preamble --transport nan --count 2
    --rate 23040000 --format sc16 --cfo 40000 --ppm 20)

# Offline decoder: frames, identity, location and summary counters.
run(0 "odid_frames=3 uas=1 .*fcs_fail=0" "${DECODE}" "${beacon}")
run(0 "phy=ofdm rate=6 .*transport=wifi_beacon .*id=SYNTH-ODID-0001 .*lat=51.47790" "${DECODE}" "${beacon}")
run(0 "phy=dsss rate=2\\(short\\) .*transport=wifi_nan .*operator=OP-SYNTH-0001" "${DECODE}" "${nan}" --format sc16)
run(0 "odid_frames=2 uas=1" "${DECODE}" "${nan}" --format sc16 --no-ofdm)
run(0 "odid_frames=0" "${DECODE}" "${nan}" --format sc16 --no-dsss)
run(3 "odid_frames=3" "${DECODE}" "${beacon}" --min-frames 4 --quiet)
run(0 "odid_frames=3" "${DECODE}" "${beacon}" --jsonl "${WORK}/odid_beacon.jsonl" --quiet)
file(STRINGS "${WORK}/odid_beacon.jsonl" lines)
list(LENGTH lines n)
if(NOT n EQUAL 3 OR NOT lines MATCHES "\"uas_id\":\"SYNTH-ODID-0001\"" OR NOT lines MATCHES "\"mpdu_hex\":\"80")
  message(FATAL_ERROR "Unexpected JSONL (${n} lines): ${lines}")
endif()
run_fails("${DECODE}")
run_fails("${DECODE}" /nonexistent-punjdrone-odid.iq)
run_fails("${DECODE}" "${beacon}" --freq-offset 2e7)
run_fails("${DECODE}" "${beacon}" --rate nan)
run_fails("${DECODE}" "${beacon}" --no-ofdm --no-dsss)
run_fails("${GEN}" "${WORK}/bad.iq" --rate 10e6)
run_fails("${GEN}" "${WORK}/bad.iq" --phy dsss --mbps 11)

# Opt-in UI mode: ODID drones reach the tracks table and packet log, tagged ODID/ASTM.
if(UI)
  run(0 "status=Disconnected packets=3 tracks=1.*track: serial=SYNTH-ODID-0001 count=3 .*source=ODID/ASTM .*gen=\"ODID/ASTM wifi_beacon\".*packet: source=ODID/ASTM serial=SYNTH-ODID-0001 seq=0 .*drone=valid lat=51.4779000"
      "${UI}" --check --odid --replay "${beacon}")
  run(0 "packets=2 tracks=1.*gen=\"ODID/ASTM wifi_nan\"" "${UI}" --check --odid --replay "${nan}" --format sc16)
  # Default (DJI) mode is unchanged: the same file yields no DJI packets.
  run(0 "packets=0 tracks=0" "${UI}" --check --replay "${beacon}")
  run_fails("${UI}" --check --odid --df --replay "${beacon}")
  run_fails("${UI}" --check --odid --auto-center --replay "${beacon}")
  run_fails("${UI}" --check --odid --rate 15.36e6 --replay "${beacon}")
endif()
file(REMOVE "${beacon}" "${beacon}.meta" "${nan}" "${nan}.meta" "${WORK}/odid_beacon.jsonl")
