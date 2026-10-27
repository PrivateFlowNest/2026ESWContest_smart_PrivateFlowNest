# hm_vision: NODE-RPI5-02 vision presence (frame sources, YOLOX-Nano via ONNX Runtime, zones, FIGURE_PRESENCE).
# Port of services/edge-ai/vision-presence (Python reference). Every dependency is optional so the configure
# never fails; missing pieces throw at runtime and the gtests skip them:
#   ONNX Runtime (HM_ONNXRUNTIME_ROOT)  inference
#   libjpeg-turbo / libpng              JPEG (files, MJPEG, Camera1 captures) and PNG decoding
#   libsystemd (sd-bus)                 Camera1 D-Bus source
# Image: homemind-native-dev:24.04 (native/docker/dev.Dockerfile) carries the last three packages.
find_package(JPEG QUIET)
find_package(PNG QUIET)
pkg_check_modules(HM_VISION_SYSTEMD QUIET IMPORTED_TARGET libsystemd)

set(_hm_vision_ort_root "${HM_ONNXRUNTIME_ROOT}")
set(_hm_vision_ort_lib "")
if(_hm_vision_ort_root AND EXISTS "${_hm_vision_ort_root}/include/onnxruntime_cxx_api.h")
  find_library(HM_VISION_ORT_LIBRARY NAMES onnxruntime PATHS "${_hm_vision_ort_root}/lib" NO_DEFAULT_PATH)
  if(HM_VISION_ORT_LIBRARY)
    set(_hm_vision_ort_lib "${HM_VISION_ORT_LIBRARY}")
  endif()
endif()

# Parity with numpy float32 / OpenCV fixed point: no FMA contraction (GCC fuses a*b+c on aarch64 by default).
set(HM_VISION_FP_FLAGS -ffp-contract=off -fno-fast-math)

add_library(hm_vision STATIC
  src/image.cpp
  src/detector.cpp
  src/background.cpp
  src/onnx_backend.cpp
  src/zones.cpp
  src/presence.cpp
  src/preview.cpp
  src/sources.cpp
  src/v4l2_source.cpp
  src/camera1_source.cpp)
target_include_directories(hm_vision PUBLIC include)
target_link_libraries(hm_vision PUBLIC hm_common)
target_compile_options(hm_vision PRIVATE ${HM_WARNINGS} ${HM_VISION_FP_FLAGS})

set(_hm_vision_features "")
if(_hm_vision_ort_lib)
  target_include_directories(hm_vision SYSTEM PRIVATE "${_hm_vision_ort_root}/include")
  target_link_libraries(hm_vision PUBLIC "${_hm_vision_ort_lib}")
  target_compile_definitions(hm_vision PRIVATE HM_VISION_HAVE_ORT=1)
  list(APPEND _hm_vision_features onnxruntime)
endif()
if(JPEG_FOUND)
  target_link_libraries(hm_vision PRIVATE JPEG::JPEG)
  target_compile_definitions(hm_vision PRIVATE HM_VISION_HAVE_JPEG=1)
  list(APPEND _hm_vision_features jpeg)
endif()
if(PNG_FOUND)
  target_link_libraries(hm_vision PRIVATE PNG::PNG)
  target_compile_definitions(hm_vision PRIVATE HM_VISION_HAVE_PNG=1)
  list(APPEND _hm_vision_features png)
endif()
if(HM_VISION_SYSTEMD_FOUND)
  target_link_libraries(hm_vision PRIVATE PkgConfig::HM_VISION_SYSTEMD)
  target_compile_definitions(hm_vision PRIVATE HM_VISION_HAVE_SDBUS=1)
  list(APPEND _hm_vision_features sd-bus)
endif()
message(STATUS "hm_vision: features = ${_hm_vision_features}")

# JSONL probe for native/tests/test_vision_*.py (parity with the Python reference).
add_executable(hm_vision_probe tools/hm_vision_probe.cpp)
target_link_libraries(hm_vision_probe PRIVATE hm_vision)
target_compile_options(hm_vision_probe PRIVATE ${HM_WARNINGS} ${HM_VISION_FP_FLAGS})

if(HM_BUILD_TESTS)
  add_executable(hm_vision_tests tests/test_vision.cpp)
  target_link_libraries(hm_vision_tests PRIVATE hm_vision GTest::gtest_main)
  target_compile_options(hm_vision_tests PRIVATE ${HM_WARNINGS} ${HM_VISION_FP_FLAGS})
  target_compile_definitions(hm_vision_tests PRIVATE HM_TEST_REPO_ROOT="${HM_REPO_ROOT}")
  add_test(NAME hm_vision_tests COMMAND hm_vision_tests)
endif()
