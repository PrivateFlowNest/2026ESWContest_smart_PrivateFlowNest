# hm_parser: NODE-RPI5-02 deterministic voice parser (port of services/runtime voice.py).
# Target name hm_parser is what services/homemind-perception links (agent B).
add_library(hm_parser STATIC src/voice_parser.cpp)
target_include_directories(hm_parser PUBLIC include)
target_link_libraries(hm_parser PUBLIC hm_common PRIVATE hm_guard hm_text)
target_compile_options(hm_parser PRIVATE ${HM_WARNINGS})

add_executable(hm_parser_probe tools/hm_parser_probe.cpp)
target_link_libraries(hm_parser_probe PRIVATE hm_parser hm_text)
target_compile_options(hm_parser_probe PRIVATE ${HM_WARNINGS})

if(HM_BUILD_TESTS)
  add_executable(hm_parser_tests tests/test_voice_parser.cpp)
  target_link_libraries(hm_parser_tests PRIVATE hm_parser hm_guard GTest::gtest_main)
  target_compile_options(hm_parser_tests PRIVATE ${HM_WARNINGS})
  target_compile_definitions(hm_parser_tests PRIVATE
    HM_TEST_CONTRACTS_DIR="${HM_CONTRACTS_DIR}"
    HM_TEST_CAPABILITIES="${HM_REPO_ROOT}/services/exaone-edge/config/device-capabilities.json")
  add_test(NAME hm_parser_tests COMMAND hm_parser_tests)
endif()
