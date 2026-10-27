# hm_partial: partial-intent hints from streaming ASR partials (NODE-RPI5-02).
# Port of services/edge-ai/partial-intent (features, ONNX runtime, two-update stream gate, safety policy).
# Needs libutf8proc (NFKC, lower-case; in the dev image). ONNX Runtime (HM_ONNXRUNTIME_ROOT) is optional:
# without it the feature code still builds and IntentRuntime throws.
pkg_check_modules(HM_PARTIAL_UTF8PROC QUIET IMPORTED_TARGET libutf8proc)
if(NOT HM_PARTIAL_UTF8PROC_FOUND)
  message(STATUS "hm_partial: skipped (libutf8proc not found)")
  return()
endif()

set(_hm_partial_ort_root "${HM_ONNXRUNTIME_ROOT}")
set(_hm_partial_ort_lib "")
if(_hm_partial_ort_root AND EXISTS "${_hm_partial_ort_root}/include/onnxruntime_cxx_api.h")
  find_library(HM_PARTIAL_ORT_LIBRARY NAMES onnxruntime PATHS "${_hm_partial_ort_root}/lib" NO_DEFAULT_PATH)
  if(HM_PARTIAL_ORT_LIBRARY)
    set(_hm_partial_ort_lib "${HM_PARTIAL_ORT_LIBRARY}")
  endif()
endif()

# float32 feature values must match numpy bit for bit: no FMA contraction (aarch64 GCC default).
set(HM_PARTIAL_FP_FLAGS -ffp-contract=off -fno-fast-math)

add_library(hm_partial STATIC src/text.cpp src/intent.cpp)
target_include_directories(hm_partial PUBLIC include)
target_link_libraries(hm_partial PUBLIC hm_common PRIVATE PkgConfig::HM_PARTIAL_UTF8PROC)
target_compile_options(hm_partial PRIVATE ${HM_WARNINGS} ${HM_PARTIAL_FP_FLAGS})
if(_hm_partial_ort_lib)
  target_include_directories(hm_partial SYSTEM PRIVATE "${_hm_partial_ort_root}/include")
  target_link_libraries(hm_partial PUBLIC "${_hm_partial_ort_lib}")
  target_compile_definitions(hm_partial PRIVATE HM_PARTIAL_HAVE_ORT=1)
  message(STATUS "hm_partial: with ONNX Runtime")
else()
  message(STATUS "hm_partial: without ONNX Runtime (IntentRuntime unavailable)")
endif()

# JSONL probe for native/tests/test_partial_*.py (parity with the Python reference, PC latency).
add_executable(hm_partial_probe tools/hm_partial_probe.cpp)
target_link_libraries(hm_partial_probe PRIVATE hm_partial)
target_compile_options(hm_partial_probe PRIVATE ${HM_WARNINGS} ${HM_PARTIAL_FP_FLAGS})

if(HM_BUILD_TESTS)
  add_executable(hm_partial_tests tests/test_partial.cpp)
  target_link_libraries(hm_partial_tests PRIVATE hm_partial GTest::gtest_main)
  target_compile_options(hm_partial_tests PRIVATE ${HM_WARNINGS} ${HM_PARTIAL_FP_FLAGS})
  target_compile_definitions(hm_partial_tests PRIVATE HM_TEST_REPO_ROOT="${HM_REPO_ROOT}"
                                                       HM_TEST_CONTRACTS_DIR="${HM_CONTRACTS_DIR}")
  add_test(NAME hm_partial_tests COMMAND hm_partial_tests)
endif()
