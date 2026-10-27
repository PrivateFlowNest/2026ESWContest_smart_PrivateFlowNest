# homemind-llm: NODE-RPI5-01 EXAONE node (port of services/runtime llm_node.py + cli.run_llm).
add_library(homemind_llm_node STATIC src/llm_node.cpp)
target_include_directories(homemind_llm_node PUBLIC src)
target_link_libraries(homemind_llm_node PUBLIC hm_guard hm_common PRIVATE hm_text)
target_compile_options(homemind_llm_node PRIVATE ${HM_WARNINGS})

add_executable(homemind-llm src/main.cpp)
target_link_libraries(homemind-llm PRIVATE homemind_llm_node)
target_compile_options(homemind-llm PRIVATE ${HM_WARNINGS})
install(TARGETS homemind-llm RUNTIME DESTINATION bin)

if(HM_BUILD_TESTS)
  add_executable(homemind_llm_tests tests/test_llm_node.cpp)
  target_link_libraries(homemind_llm_tests PRIVATE homemind_llm_node GTest::gtest_main)
  target_compile_options(homemind_llm_tests PRIVATE ${HM_WARNINGS})
  target_compile_definitions(homemind_llm_tests PRIVATE
    HM_TEST_CONTRACTS_DIR="${HM_CONTRACTS_DIR}"
    HM_TEST_CAPABILITIES="${HM_REPO_ROOT}/services/exaone-edge/config/device-capabilities.json")
  add_test(NAME homemind_llm_tests COMMAND homemind_llm_tests)
endif()
