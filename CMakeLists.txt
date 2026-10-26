# punjdrone — DJI DroneID passive receiver (X310 / UBX-160).
# GPL-3.0-or-later. See DRONEID_BUILD_PLAN.md for the full plan.
cmake_minimum_required(VERSION 3.16)
project(droneid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

# Feature toggles — OFF items are future phases; Phase 0 needs none of them.
option(DRONEID_WITH_UHD    "Build the UHD radio source (Phase 1+)"        OFF)
option(DRONEID_WITH_UI     "Build the Dear ImGui operator UI (Phase 3+)"  OFF)
option(DRONEID_WITH_SQLITE "Build the SQLite sink (Phase 6+)"            OFF)
option(DRONEID_WITH_GTEST  "Use system GoogleTest instead of microtest"  OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig)

# --- core library (Phase 0) ---
add_library(droneid_core
  src/core/config.cpp
  src/sdr/file_source.cpp)
target_include_directories(droneid_core PUBLIC include)
target_link_libraries(droneid_core PUBLIC Threads::Threads)

# --- application ---
add_executable(droneid src/app/main.cpp)
target_link_libraries(droneid PRIVATE droneid_core)

# --- tools (Phase 0) ---
add_executable(iq_replay tools/iq_replay.cpp)
target_link_libraries(iq_replay PRIVATE droneid_core)

add_executable(iq_gen tools/iq_gen.cpp)
target_link_libraries(iq_gen PRIVATE droneid_core)

add_executable(channelize tools/channelize.cpp)
target_link_libraries(channelize PRIVATE droneid_core)

# --- UHD radio source + iq_record (Phase 1) ---
if(DRONEID_WITH_UHD)
  find_package(UHD QUIET)
  if(NOT UHD_FOUND AND PkgConfig_FOUND)
    pkg_check_modules(UHD uhd)
  endif()
  if(NOT UHD_FOUND)
    message(FATAL_ERROR "DRONEID_WITH_UHD=ON but UHD not found")
  endif()
  find_package(Boost REQUIRED)  # UHD headers pull in a few Boost headers
  add_library(droneid_sdr src/sdr/usrp_source.cpp)
  target_include_directories(droneid_sdr PUBLIC include ${UHD_INCLUDE_DIRS})
  target_link_libraries(droneid_sdr PUBLIC droneid_core ${UHD_LIBRARIES} Boost::boost)
  target_link_directories(droneid_sdr PUBLIC ${UHD_LIBRARY_DIRS})

  add_executable(iq_record tools/iq_record.cpp)
  target_link_libraries(iq_record PRIVATE droneid_sdr)
  # libuhd.so references Python/Boost/DPDK symbols from its own NEEDED libs; let them
  # resolve at runtime instead of erroring at link (needed with the linuxbrew linker).
  target_link_options(iq_record PRIVATE -Wl,--allow-shlib-undefined)
  message(STATUS "droneid: UHD found (${UHD_VERSION})")
endif()

# --- vendored turbofec (GPLv3) for LTE turbo + rate matching (Phase 2) ---
file(GLOB TURBOFEC_SRC third_party/turbofec/src/*.c)
add_library(turbofec STATIC ${TURBOFEC_SRC})
target_include_directories(turbofec PUBLIC third_party/turbofec/include)
set_target_properties(turbofec PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(turbofec PRIVATE -w)  # upstream code, silence its warnings
# turbofec's turbo decoder core is ONLY defined under HAVE_SSE3 (no generic fallback);
# without it the decode is a silent no-op. Enable SSE on x86.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i686|i386")
  target_compile_definitions(turbofec PRIVATE HAVE_SSE3 HAVE_SSE4_1)
  target_compile_options(turbofec PRIVATE -mssse3 -msse4.1)
else()
  message(WARNING "turbofec: non-x86 target — turbo decoder needs a SIMD path; decode may not work")
endif()

# --- DJI DroneID decoder library (Phase 2, needs FFTW3f) ---
if(PkgConfig_FOUND)
  pkg_check_modules(FFTW3F fftw3f)
  if(FFTW3F_FOUND)
    add_library(droneid_dji
      src/dji/params.cpp src/dji/dsp.cpp src/dji/decode.cpp
      src/fec/scrambler.cpp src/fec/turbo.cpp src/proto/frame.cpp)
    target_include_directories(droneid_dji PUBLIC include
      PRIVATE ${FFTW3F_INCLUDE_DIRS} third_party/crcpp)
    target_link_libraries(droneid_dji PUBLIC droneid_core
      PRIVATE turbofec ${FFTW3F_LIBRARIES})
    target_link_directories(droneid_dji PRIVATE ${FFTW3F_LIBRARY_DIRS})

    add_executable(droneid_decode tools/droneid_decode.cpp)
    target_link_libraries(droneid_decode PRIVATE droneid_dji)

    add_executable(frame_gen tools/frame_gen.cpp)
    target_link_libraries(frame_gen PRIVATE droneid_dji)
  endif()
endif()

# --- burst_scan analyzer (Phase 1, needs FFTW3f) ---
if(PkgConfig_FOUND)
  if(FFTW3F_FOUND)
    add_executable(burst_scan tools/burst_scan.cpp)
    target_include_directories(burst_scan PRIVATE ${FFTW3F_INCLUDE_DIRS})
    target_link_libraries(burst_scan PRIVATE droneid_core ${FFTW3F_LIBRARIES})
    target_link_directories(burst_scan PRIVATE ${FFTW3F_LIBRARY_DIRS})

    pkg_check_modules(ZLIB zlib)
    if(ZLIB_FOUND)
      add_executable(spectrogram tools/spectrogram.cpp)
      target_include_directories(spectrogram PRIVATE ${FFTW3F_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS})
      target_link_libraries(spectrogram PRIVATE droneid_core ${FFTW3F_LIBRARIES} ${ZLIB_LIBRARIES})
      target_link_directories(spectrogram PRIVATE ${FFTW3F_LIBRARY_DIRS} ${ZLIB_LIBRARY_DIRS})
    endif()
  else()
    message(STATUS "droneid: fftw3f not found — skipping burst_scan")
  endif()
endif()

# --- tests ---
enable_testing()
add_subdirectory(tests)

message(STATUS "droneid: UHD=${DRONEID_WITH_UHD} UI=${DRONEID_WITH_UI} "
               "SQLITE=${DRONEID_WITH_SQLITE} GTEST=${DRONEID_WITH_GTEST}")
