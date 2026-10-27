add_library(hm_common STATIC
  src/common.cpp
  src/config.cpp
  src/mqtt.cpp
  src/pfn.cpp
  src/action_map.cpp
  src/heartbeat.cpp
  src/http.cpp)
target_include_directories(hm_common PUBLIC include)
target_link_libraries(hm_common PUBLIC nlohmann_json::nlohmann_json PkgConfig::MOSQUITTO Threads::Threads)
target_compile_options(hm_common PRIVATE ${HM_WARNINGS})
target_compile_definitions(hm_common PRIVATE
  HM_CONTRACTS_DIR_DEFAULT="${HM_CONTRACTS_DIR}"
  HM_REPO_ROOT_DEFAULT="${HM_REPO_ROOT}")

if(HM_BUILD_TESTS)
  add_executable(hm_common_tests tests/test_common.cpp)
  target_link_libraries(hm_common_tests PRIVATE hm_common GTest::gtest_main)
  target_compile_options(hm_common_tests PRIVATE ${HM_WARNINGS})
  target_compile_definitions(hm_common_tests PRIVATE HM_TEST_CONTRACTS_DIR="${HM_CONTRACTS_DIR}")
  add_test(NAME hm_common_tests COMMAND hm_common_tests)
endif()

add_executable(hm_common_probe tools/hm_common_probe.cpp)
target_link_libraries(hm_common_probe PRIVATE hm_common)
target_compile_options(hm_common_probe PRIVATE ${HM_WARNINGS})
