# homemind-voice-text: NODE-RPI5-02 keyboard fallback (port of services/runtime cli.run_voice_text + ContextTracker).
add_library(homemind_voice_text_core STATIC src/context_tracker.cpp)
target_include_directories(homemind_voice_text_core PUBLIC src)
target_link_libraries(homemind_voice_text_core PUBLIC hm_common PRIVATE hm_text)
target_compile_options(homemind_voice_text_core PRIVATE ${HM_WARNINGS})

add_executable(homemind-voice-text src/main.cpp)
target_link_libraries(homemind-voice-text PRIVATE homemind_voice_text_core hm_parser hm_guard hm_text)
target_compile_options(homemind-voice-text PRIVATE ${HM_WARNINGS})
install(TARGETS homemind-voice-text RUNTIME DESTINATION bin)

if(HM_BUILD_TESTS)
  add_executable(homemind_voice_text_tests tests/test_context_tracker.cpp)
  target_link_libraries(homemind_voice_text_tests PRIVATE homemind_voice_text_core GTest::gtest_main)
  target_compile_options(homemind_voice_text_tests PRIVATE ${HM_WARNINGS})
  add_test(NAME homemind_voice_text_tests COMMAND homemind_voice_text_tests)
endif()
