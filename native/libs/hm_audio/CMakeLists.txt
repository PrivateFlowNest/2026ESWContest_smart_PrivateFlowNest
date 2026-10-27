# hm_audio: NODE-RPI5-02 audio input (WAV I/O, scipy-compatible resampler, energy VAD, ring buffer fan-out,
# ALSA capture, push-to-talk triggers). Port of services/runtime audio/{capture,vad}.py.
pkg_check_modules(ALSA IMPORTED_TARGET alsa)

add_library(hm_audio STATIC
  src/numeric.cpp
  src/wav.cpp
  src/resample.cpp
  src/vad.cpp
  src/ring.cpp
  src/ptt.cpp
  src/capture.cpp)
target_include_directories(hm_audio PUBLIC include)
target_link_libraries(hm_audio PUBLIC hm_common Threads::Threads)
if(ALSA_FOUND)
  target_link_libraries(hm_audio PUBLIC PkgConfig::ALSA)
  target_compile_definitions(hm_audio PRIVATE HM_HAVE_ALSA=1)
else()
  message(WARNING "hm_audio: ALSA (libasound2-dev) not found; microphone capture disabled")
endif()
# No fused multiply-add contraction (GCC's default on aarch64 in GNU mode): keeps VAD levels and the
# resampler bit-compatible with the numpy/scipy reference on both architectures.
target_compile_options(hm_audio PRIVATE ${HM_WARNINGS} -ffp-contract=off)

# JSONL probe for the Python parity tests (native/tests/test_perception_audio.py).
add_executable(hm_audio_probe tools/hm_audio_probe.cpp)
target_link_libraries(hm_audio_probe PRIVATE hm_audio)
target_compile_options(hm_audio_probe PRIVATE ${HM_WARNINGS})

if(HM_BUILD_TESTS)
  add_executable(hm_audio_tests tests/test_audio.cpp)
  target_link_libraries(hm_audio_tests PRIVATE hm_audio GTest::gtest_main)
  target_compile_options(hm_audio_tests PRIVATE ${HM_WARNINGS})
  add_test(NAME hm_audio_tests COMMAND hm_audio_tests)
endif()
