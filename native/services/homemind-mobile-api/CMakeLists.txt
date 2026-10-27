# homemind-mobile-api: phone app HTTP API on NODE-RPI4-01 (C++ port of team/homemind-application-2026-10-21/backend,
# Mobile API 0.4: pfn.v1 core path + QR pairing tokens). HTTP server: vendored cpp-httplib (native/third_party).
set(HM_HTTPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cpp-httplib")
# HTTPS (tls_enabled) needs OpenSSL 3 headers (libssl-dev in native/docker/dev.Dockerfile). The device has libssl.so.3
# already (core's libmosquitto uses it), so packages do not bundle it. Without OpenSSL the build has no HTTPS.
find_package(OpenSSL 3 QUIET)
if(NOT TARGET hm_cpp_httplib)
  add_library(hm_cpp_httplib STATIC "${HM_HTTPLIB_DIR}/httplib.cpp")
  target_include_directories(hm_cpp_httplib SYSTEM PUBLIC "${HM_HTTPLIB_DIR}")
  target_link_libraries(hm_cpp_httplib PUBLIC Threads::Threads)
  target_compile_options(hm_cpp_httplib PRIVATE -w)  # third-party code: no warnings, no -Werror
  if(OpenSSL_FOUND)
    target_compile_definitions(hm_cpp_httplib PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(hm_cpp_httplib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  endif()
endif()

set(HM_QRCODEGEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/qrcodegen")
if(NOT TARGET hm_qrcodegen)
  add_library(hm_qrcodegen STATIC "${HM_QRCODEGEN_DIR}/qrcodegen.cpp")
  target_include_directories(hm_qrcodegen SYSTEM PUBLIC "${HM_QRCODEGEN_DIR}")
  target_compile_options(hm_qrcodegen PRIVATE -w)  # third-party code (Nayuki, MIT): no warnings, no -Werror
endif()

add_library(homemind_mobile_api_core STATIC
  src/api.cpp
  src/approvals.cpp
  src/auth.cpp
  src/core_bridge.cpp
  src/crypto.cpp
  src/pairing_relay.cpp
  src/phones_relay.cpp
  src/registry.cpp
  src/service_config.cpp
  src/store.cpp
  src/terminal_qr.cpp
  src/tls.cpp)
target_include_directories(homemind_mobile_api_core PUBLIC src)
target_link_libraries(homemind_mobile_api_core PUBLIC hm_common hm_qrcodegen)
if(OpenSSL_FOUND)
  target_compile_definitions(homemind_mobile_api_core PUBLIC HM_MOBILE_API_TLS)
  target_link_libraries(homemind_mobile_api_core PUBLIC OpenSSL::Crypto)
else()
  message(STATUS "homemind-mobile-api: OpenSSL 3 not found, building without HTTPS")
endif()
target_compile_options(homemind_mobile_api_core PRIVATE ${HM_WARNINGS})

add_executable(homemind-mobile-api src/main.cpp)
target_link_libraries(homemind-mobile-api PRIVATE homemind_mobile_api_core hm_cpp_httplib)
target_compile_options(homemind-mobile-api PRIVATE ${HM_WARNINGS})
install(TARGETS homemind-mobile-api RUNTIME DESTINATION bin)

if(HM_BUILD_TESTS)
  add_executable(homemind_mobile_api_tests tests/test_mobile_api.cpp)
  target_link_libraries(homemind_mobile_api_tests PRIVATE homemind_mobile_api_core GTest::gtest_main)
  target_compile_options(homemind_mobile_api_tests PRIVATE ${HM_WARNINGS})
  target_compile_definitions(homemind_mobile_api_tests PRIVATE
    HM_TEST_CONTRACTS_DIR="${HM_CONTRACTS_DIR}"
    HM_TEST_SERVICE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME homemind_mobile_api_tests COMMAND homemind_mobile_api_tests)
endif()
