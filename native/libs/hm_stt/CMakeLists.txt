# hm_stt: whisper.cpp transcription (whisper-server HTTP, whisper-cli fallback). Port of services/runtime audio/stt.py.
add_library(hm_stt STATIC src/stt.cpp src/text.cpp)
target_include_directories(hm_stt PUBLIC include)
target_link_libraries(hm_stt PUBLIC hm_common hm_audio)
target_compile_options(hm_stt PRIVATE ${HM_WARNINGS})

# PC integration probe: transcribe WAV files through a real whisper-server / whisper-cli.
add_executable(hm_stt_probe tools/hm_stt_probe.cpp)
target_link_libraries(hm_stt_probe PRIVATE hm_stt)
target_compile_options(hm_stt_probe PRIVATE ${HM_WARNINGS})

if(HM_BUILD_TESTS)
  add_executable(hm_stt_tests tests/test_stt.cpp)
  target_link_libraries(hm_stt_tests PRIVATE hm_stt GTest::gtest_main)
  target_compile_options(hm_stt_tests PRIVATE ${HM_WARNINGS})
  add_test(NAME hm_stt_tests COMMAND hm_stt_tests)
endif()
