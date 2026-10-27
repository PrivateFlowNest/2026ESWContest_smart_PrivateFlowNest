# HomeMind native services for the RPI5 nodes (LG AI Native OS has no Python/Docker).
# Same pfn.v1 contracts as services/runtime (Python reference, also the parity oracle).
cmake_minimum_required(VERSION 3.20)
project(homemind_native VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(HM_BUILD_TESTS "unit tests (gtest)" ON)
set(HM_ONNXRUNTIME_ROOT "$ENV{HM_ONNXRUNTIME_ROOT}" CACHE PATH "extracted onnxruntime-linux-<arch>-1.20.1 (include/, lib/)")
# A build tree first configured without the variable keeps the empty cache entry; take the environment then.
if(NOT HM_ONNXRUNTIME_ROOT AND NOT "$ENV{HM_ONNXRUNTIME_ROOT}" STREQUAL "")
  set(HM_ONNXRUNTIME_ROOT "$ENV{HM_ONNXRUNTIME_ROOT}" CACHE PATH "extracted onnxruntime-linux-<arch>-1.20.1 (include/, lib/)" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
if(HM_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
endif()

set(HM_WARNINGS -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Werror)
get_filename_component(HM_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(HM_CONTRACTS_DIR "${HM_REPO_ROOT}/contracts" CACHE PATH "contracts/ (pfn.v1, rpi5-internal)")

# Each library/service owns its folder; targets may link each other in any order.
add_subdirectory(libs/hm_common)
file(GLOB hm_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/libs/*" "${CMAKE_CURRENT_SOURCE_DIR}/services/*")
list(SORT hm_dirs)
foreach(dir IN LISTS hm_dirs)
  if(EXISTS "${dir}/CMakeLists.txt" AND NOT dir MATCHES "/hm_common$")
    add_subdirectory("${dir}")
  endif()
endforeach()
