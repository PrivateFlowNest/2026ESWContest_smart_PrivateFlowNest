# hm_context: audio context path (YAMNet via ONNX Runtime, VAD features, activity head, context fusion).
# Needs ONNX Runtime 1.20.1 (HM_ONNXRUNTIME_ROOT with include/ and lib/); without it the library is skipped.
set(_hm_ort_root "${HM_ONNXRUNTIME_ROOT}")
if(NOT _hm_ort_root OR NOT EXISTS "${_hm_ort_root}/include/onnxruntime_cxx_api.h")
  message(STATUS "hm_context: skipped (HM_ONNXRUNTIME_ROOT='${_hm_ort_root}' has no ONNX Runtime headers)")
  return()
endif()
find_library(HM_CONTEXT_ORT_LIBRARY NAMES onnxruntime PATHS "${_hm_ort_root}/lib" NO_DEFAULT_PATH)
if(NOT HM_CONTEXT_ORT_LIBRARY)
  message(STATUS "hm_context: skipped (no libonnxruntime in ${_hm_ort_root}/lib)")
  return()
endif()

# Bit-level parity with NumPy/Python needs IEEE evaluation without FMA contraction (aarch64 would fuse a*b+c).
set(HM_CONTEXT_FP_FLAGS -ffp-contract=off -fno-fast-math)

add_library(hm_context STATIC
  src/pyfmt.cpp
  src/vad.cpp
  src/yamnet.cpp
  src/activity_head.cpp
  src/fusion.cpp
  src/context_pipeline.cpp)
target_include_directories(hm_context PUBLIC include)
target_include_directories(hm_context SYSTEM PRIVATE "${_hm_ort_root}/include")
target_link_libraries(hm_context PUBLIC hm_common "${HM_CONTEXT_ORT_LIBRARY}")
target_compile_options(hm_context PRIVATE ${HM_WARNINGS} ${HM_CONTEXT_FP_FLAGS})

add_executable(hm_context_probe tools/hm_context_probe.cpp)
target_link_libraries(hm_context_probe PRIVATE hm_context)
target_compile_options(hm_context_probe PRIVATE ${HM_WARNINGS} ${HM_CONTEXT_FP_FLAGS})

if(HM_BUILD_TESTS)
  add_executable(hm_context_tests tests/test_context.cpp)
  target_link_libraries(hm_context_tests PRIVATE hm_context GTest::gtest_main)
  target_compile_options(hm_context_tests PRIVATE ${HM_WARNINGS} ${HM_CONTEXT_FP_FLAGS})
  target_compile_definitions(hm_context_tests PRIVATE HM_TEST_REPO_ROOT="${HM_REPO_ROOT}")
  add_test(NAME hm_context_tests COMMAND hm_context_tests)
endif()
