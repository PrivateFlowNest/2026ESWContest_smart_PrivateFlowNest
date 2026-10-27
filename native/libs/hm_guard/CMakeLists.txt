# hm_guard: deterministic guard, proposal validation and the EXAONE (llama-server) client.
# Port of services/exaone-edge (guardrails.py, validation.py, client.py); parity: native/tests/test_guard_parity.py
add_library(hm_guard STATIC
  src/guardrails.cpp
  src/validation.cpp
  src/exaone_client.cpp
  src/pfn_schema.cpp)
target_include_directories(hm_guard PUBLIC include)
target_link_libraries(hm_guard PUBLIC hm_common hm_text)
target_compile_options(hm_guard PRIVATE ${HM_WARNINGS})
target_compile_definitions(hm_guard PRIVATE
  HM_CAPABILITIES_DEFAULT="${HM_REPO_ROOT}/services/exaone-edge/config/device-capabilities.json")

add_executable(hm_guard_probe tools/hm_guard_probe.cpp)
target_link_libraries(hm_guard_probe PRIVATE hm_guard)
target_compile_options(hm_guard_probe PRIVATE ${HM_WARNINGS})

if(HM_BUILD_TESTS)
  add_executable(hm_guard_tests tests/test_guardrails.cpp tests/test_validation.cpp tests/test_client.cpp)
  target_link_libraries(hm_guard_tests PRIVATE hm_guard GTest::gtest_main)
  target_compile_options(hm_guard_tests PRIVATE ${HM_WARNINGS})
  target_compile_definitions(hm_guard_tests PRIVATE
    HM_TEST_CONTRACTS_DIR="${HM_CONTRACTS_DIR}"
    HM_TEST_CAPABILITIES="${HM_REPO_ROOT}/services/exaone-edge/config/device-capabilities.json")
  add_test(NAME hm_guard_tests COMMAND hm_guard_tests)
endif()
