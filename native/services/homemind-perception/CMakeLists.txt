# homemind-perception (NODE-RPI5-02): port of services/runtime perception.py.
# Links libs/hm_context (agent C) and libs/hm_parser (agent A) when those targets exist; without them the
# binary still builds and reports the missing path at start-up (tests use doubles).
option(HM_PERCEPTION_WITH_CONTEXT "link libs/hm_context when available" ON)
option(HM_PERCEPTION_WITH_PARSER "link libs/hm_parser when available" ON)
option(HM_PERCEPTION_WITH_PARTIAL "link libs/hm_vosk + libs/hm_partial (partial-intent hints) when available" ON)

add_library(hm_perception_core STATIC src/settings.cpp src/engines.cpp src/service.cpp src/runner.cpp
            src/voice_listen.cpp src/presence_feed.cpp src/partial_hints.cpp)
target_include_directories(hm_perception_core PUBLIC src)
target_link_libraries(hm_perception_core PUBLIC hm_common hm_audio hm_stt Threads::Threads)
target_compile_options(hm_perception_core PRIVATE ${HM_WARNINGS})

if(HM_PERCEPTION_WITH_CONTEXT AND TARGET hm_context)
  target_link_libraries(hm_perception_core PUBLIC hm_context)
  target_compile_definitions(hm_perception_core PRIVATE HM_PERCEPTION_HAVE_CONTEXT=1)
  message(STATUS "homemind-perception: context path = libs/hm_context")
else()
  target_include_directories(hm_perception_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/hm_context/include")
  message(STATUS "homemind-perception: hm_context not linked (context path unavailable)")
endif()
if(HM_PERCEPTION_WITH_PARSER AND TARGET hm_parser)
  target_link_libraries(hm_perception_core PUBLIC hm_parser)
  target_compile_definitions(hm_perception_core PRIVATE HM_PERCEPTION_HAVE_PARSER=1)
  message(STATUS "homemind-perception: voice decisions = libs/hm_parser")
else()
  target_include_directories(hm_perception_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/hm_parser/include")
  message(STATUS "homemind-perception: hm_parser not linked (voice decisions unavailable)")
endif()

if(TARGET hm_guard)  # jsonschema-compatible uuid format for pfn/v1/ai/voice/listen
  target_link_libraries(hm_perception_core PRIVATE hm_guard)
  target_compile_definitions(hm_perception_core PRIVATE HM_PERCEPTION_HAVE_GUARD=1)
endif()
if(HM_PERCEPTION_WITH_PARTIAL AND TARGET hm_vosk AND TARGET hm_partial)
  target_link_libraries(hm_perception_core PUBLIC hm_vosk hm_partial)
  target_compile_definitions(hm_perception_core PRIVATE HM_PERCEPTION_HAVE_PARTIAL=1)
  message(STATUS "homemind-perception: partial-intent hints = libs/hm_vosk (dlopen libvosk) + libs/hm_partial")
else()
  message(STATUS "homemind-perception: hm_vosk/hm_partial not linked (partial-intent hints unavailable)")
endif()

add_executable(homemind-perception src/main.cpp)
target_link_libraries(homemind-perception PRIVATE hm_perception_core)
target_compile_options(homemind-perception PRIVATE ${HM_WARNINGS})
install(TARGETS homemind-perception RUNTIME DESTINATION bin)

# PC harness: the same program plus test doubles (context replay, fake STT, keyword decider). Never shipped.
add_executable(homemind-perception-harness tools/perception_harness.cpp tools/doubles.cpp)
target_link_libraries(homemind-perception-harness PRIVATE hm_perception_core)
target_include_directories(homemind-perception-harness PRIVATE tools)
target_compile_options(homemind-perception-harness PRIVATE ${HM_WARNINGS})

# PC probe (JSONL): the voice/listen schema check, for the jsonschema parity test. Never shipped.
add_executable(homemind-perception-probe tools/perception_probe.cpp)
target_link_libraries(homemind-perception-probe PRIVATE hm_perception_core)
target_compile_options(homemind-perception-probe PRIVATE ${HM_WARNINGS})

if(HM_BUILD_TESTS)
  add_executable(homemind_perception_tests tests/test_perception.cpp tests/test_live_inputs.cpp tools/doubles.cpp)
  target_link_libraries(homemind_perception_tests PRIVATE hm_perception_core GTest::gtest_main)
  target_include_directories(homemind_perception_tests PRIVATE tools)
  target_compile_options(homemind_perception_tests PRIVATE ${HM_WARNINGS})
  target_compile_definitions(homemind_perception_tests PRIVATE HM_TEST_REPO_ROOT="${HM_REPO_ROOT}")
  if(DEFINED HM_VOSK_TEST_LIBRARY)
    target_compile_definitions(homemind_perception_tests PRIVATE HM_TEST_VOSK_LIBRARY="${HM_VOSK_TEST_LIBRARY}")
  endif()
  add_test(NAME homemind_perception_tests COMMAND homemind_perception_tests)
endif()
