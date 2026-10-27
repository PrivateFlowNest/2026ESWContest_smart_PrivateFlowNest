# homemind-clock-sync: both RPI5 nodes take core's clock (pfn/v1/core/state/response) at boot and every 10 minutes.
add_library(homemind_clock_sync_core STATIC src/clock_offset.cpp)
target_include_directories(homemind_clock_sync_core PUBLIC src)
target_compile_options(homemind_clock_sync_core PRIVATE ${HM_WARNINGS})

add_executable(homemind-clock-sync src/main.cpp)
target_link_libraries(homemind-clock-sync PRIVATE homemind_clock_sync_core hm_common)
target_compile_options(homemind-clock-sync PRIVATE ${HM_WARNINGS})
install(TARGETS homemind-clock-sync RUNTIME DESTINATION bin)

if(HM_BUILD_TESTS)
  add_executable(homemind_clock_sync_tests tests/test_clock_offset.cpp)
  target_link_libraries(homemind_clock_sync_tests PRIVATE homemind_clock_sync_core GTest::gtest_main)
  target_compile_options(homemind_clock_sync_tests PRIVATE ${HM_WARNINGS})
  add_test(NAME homemind_clock_sync_tests COMMAND homemind_clock_sync_tests)
endif()
