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
option(DRONEID_UHD_DYNAMIC "Load UHD C API at runtime (Windows default)" ${WIN32})
option(DRONEID_PORTABLE_FFT "Use bundled FFT instead of system FFTW" ${WIN32})
set(DRONEID_GLFW_SOURCE "" CACHE PATH "Optional GLFW source directory")
set(DRONEID_FFTW_ROOT "" CACHE PATH "Optional target-platform FFTW install prefix")

function(droneid_runtime_link target)
  if(UNIX AND NOT APPLE)
    target_link_options(${target} PRIVATE -Wl,--allow-shlib-undefined)
  endif()
endfunction()
if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
  add_compile_definitions(_USE_MATH_DEFINES)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig)

# --- core library (Phase 0) ---
add_library(droneid_core
  src/core/config.cpp
  src/core/hop.cpp
  src/o4/aoa.cpp
  src/sdr/file_source.cpp)
target_include_directories(droneid_core PUBLIC include)
target_link_libraries(droneid_core PUBLIC Threads::Threads)

# --- ASTM F3411 Open Drone ID: message parser + Wi-Fi Beacon/NAN receiver (no FFTW needed) ---
add_library(droneid_odid
  src/odid/messages.cpp
  src/odid/wifi_mac.cpp
  src/odid/wifi_dsp.cpp
  src/odid/wifi_ofdm.cpp
  src/odid/wifi_dsss.cpp
  src/odid/wifi_decoder.cpp)
target_include_directories(droneid_odid PUBLIC include)
target_link_libraries(droneid_odid PUBLIC droneid_core)

add_executable(odid_decode tools/odid_decode.cpp)
target_link_libraries(odid_decode PRIVATE droneid_odid)
add_executable(odid_gen tools/odid_gen.cpp)
target_link_libraries(odid_gen PRIVATE droneid_odid)

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
  if(DRONEID_UHD_DYNAMIC)
    add_library(droneid_sdr src/sdr/usrp_source_c.cpp)
    target_include_directories(droneid_sdr PUBLIC include PRIVATE third_party/uhd_c)
    target_link_libraries(droneid_sdr PUBLIC droneid_core PRIVATE ${CMAKE_DL_LIBS})
  else()
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
  endif()

  add_executable(iq_record tools/iq_record.cpp)
  target_link_libraries(iq_record PRIVATE droneid_sdr)
  # libuhd.so references Python/Boost/DPDK symbols from its own NEEDED libs; let them
  # resolve at runtime instead of erroring at link (needed with the linuxbrew linker).
  droneid_runtime_link(iq_record)
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
if(DRONEID_FFTW_ROOT)
  find_path(FFTW3F_INCLUDE_DIRS fftw3.h PATHS "${DRONEID_FFTW_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(FFTW3F_LIBRARIES NAMES fftw3f PATHS "${DRONEID_FFTW_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  set(FFTW3F_FOUND TRUE)
elseif(PkgConfig_FOUND AND NOT DRONEID_PORTABLE_FFT)
  pkg_check_modules(FFTW3F fftw3f)
endif()
    add_library(droneid_dji
      src/dji/params.cpp src/dji/dsp.cpp src/dji/decode.cpp
      src/fec/scrambler.cpp src/fec/turbo.cpp src/proto/frame.cpp)
    target_include_directories(droneid_dji PUBLIC include
      PRIVATE ${FFTW3F_INCLUDE_DIRS} third_party/crcpp)
    target_link_libraries(droneid_dji PUBLIC droneid_core
      PRIVATE turbofec ${FFTW3F_LIBRARIES})
    target_link_directories(droneid_dji PRIVATE ${FFTW3F_LIBRARY_DIRS})
    target_compile_definitions(droneid_dji PRIVATE DRONEID_USE_FFTW3=$<BOOL:${FFTW3F_FOUND}>)

    add_executable(droneid_decode tools/droneid_decode.cpp)
    target_link_libraries(droneid_decode PRIVATE droneid_dji)

    add_executable(frame_gen tools/frame_gen.cpp)
    target_link_libraries(frame_gen PRIVATE droneid_dji)

    add_library(droneid_rf src/o4/rf_metrics.cpp src/o4/o4_detector.cpp)
    target_include_directories(droneid_rf PUBLIC include)
    target_link_libraries(droneid_rf PUBLIC droneid_dji)
    add_executable(rf_analyze tools/rf_analyze.cpp)
    target_link_libraries(rf_analyze PRIVATE droneid_rf)
    add_executable(o4_detect tools/o4_detect.cpp)
    target_link_libraries(o4_detect PRIVATE droneid_rf)
    add_executable(droneid_hop tools/droneid_hop.cpp)
    target_link_libraries(droneid_hop PRIVATE droneid_dji)
    if(TARGET droneid_sdr)
      target_compile_definitions(droneid_hop PRIVATE DRONEID_HAVE_UHD=1)
      target_link_libraries(droneid_hop PRIVATE droneid_sdr)
      droneid_runtime_link(droneid_hop)
    endif()

# --- burst_scan analyzer (Phase 1, needs FFTW3f) ---
if(PkgConfig_FOUND)
  if(FFTW3F_FOUND)
    add_executable(burst_scan tools/burst_scan.cpp)
    target_include_directories(burst_scan PRIVATE ${FFTW3F_INCLUDE_DIRS})
    target_link_libraries(burst_scan PRIVATE droneid_core ${FFTW3F_LIBRARIES})
    target_link_directories(burst_scan PRIVATE ${FFTW3F_LIBRARY_DIRS})

    if(NOT CMAKE_CROSSCOMPILING)
      pkg_check_modules(ZLIB zlib)
    else()
      set(ZLIB_FOUND FALSE)
    endif()
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

# --- operator UI (Phase 3) ---
# Backend-independent model library is always built so its unit test runs in CI without a GPU.
add_library(droneid_ui_model src/ui/ui_model.cpp)
target_include_directories(droneid_ui_model PUBLIC src/ui)

if(TARGET droneid_rf)
  add_library(droneid_ui_worker src/ui/receiver_worker.cpp src/ui/odid_worker.cpp)
  target_include_directories(droneid_ui_worker PUBLIC src/ui)
  target_link_libraries(droneid_ui_worker PUBLIC droneid_ui_model droneid_rf droneid_odid Threads::Threads)
  if(TARGET droneid_sdr)
    target_compile_definitions(droneid_ui_worker PRIVATE DRONEID_HAVE_UHD=1)
    target_link_libraries(droneid_ui_worker PRIVATE droneid_sdr)
  endif()
endif()

if(DRONEID_WITH_UI)
  if(NOT TARGET droneid_dji)
    message(FATAL_ERROR "DRONEID_WITH_UI needs the decoder library (FFTW3f) which was not found")
  endif()
  find_package(OpenGL REQUIRED)
  if(DRONEID_GLFW_SOURCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${DRONEID_GLFW_SOURCE} glfw)
    set(GLFW_LIBRARIES glfw)
  elseif(NOT PkgConfig_FOUND)
    message(FATAL_ERROR "DRONEID_WITH_UI needs pkg-config to locate GLFW")
  else()
    pkg_check_modules(GLFW REQUIRED glfw3)
  endif()

  # Vendored Dear ImGui (docking) + GLFW/OpenGL3 backends. Upstream code: silence its warnings.
  add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC
    third_party/imgui third_party/imgui/backends ${GLFW_INCLUDE_DIRS})
  target_compile_options(imgui PRIVATE -w)
  target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES})
  set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_executable(droneid_ui src/ui/app.cpp)
  target_include_directories(droneid_ui PRIVATE src/ui ${GLFW_INCLUDE_DIRS})
  target_link_libraries(droneid_ui PRIVATE
    droneid_ui_worker droneid_dji imgui ${GLFW_LIBRARIES} OpenGL::GL ${CMAKE_DL_LIBS})
  target_link_directories(droneid_ui PRIVATE ${GLFW_LIBRARY_DIRS})
  if(TARGET droneid_rf)   # RF metrics (EnergyBurstDetector / measure_rf) for the drones panel
    target_link_libraries(droneid_ui PRIVATE droneid_rf)
  endif()
  if(TARGET droneid_sdr)  # live radio source (Connect drives UsrpSource) when UHD is built
    target_compile_definitions(droneid_ui PRIVATE DRONEID_HAVE_UHD=1)
    target_link_libraries(droneid_ui PRIVATE droneid_sdr)
  endif()
  # GLFW's X11 deps and GLVND dispatch symbols resolve at runtime via their NEEDED libs;
  # let the (linuxbrew) linker defer them, as the UHD targets already do.
  droneid_runtime_link(droneid_ui)
  if(WIN32)
    target_link_libraries(droneid_ui PRIVATE comdlg32)
  endif()
  message(STATUS "droneid: UI enabled (GLFW ${GLFW_VERSION})")
endif()

# --- tests ---
add_executable(df_bearing tools/df_bearing.cpp)
target_link_libraries(df_bearing PRIVATE droneid_core)
if(TARGET droneid_sdr)
  add_executable(iq_record_pair tools/iq_record_pair.cpp)
  target_link_libraries(iq_record_pair PRIVATE droneid_sdr)
  droneid_runtime_link(iq_record_pair)
endif()
enable_testing()
add_subdirectory(tests)

message(STATUS "droneid: UHD=${DRONEID_WITH_UHD} UI=${DRONEID_WITH_UI} "
               "SQLITE=${DRONEID_WITH_SQLITE} GTEST=${DRONEID_WITH_GTEST}")
