cmake_minimum_required(VERSION 3.20)
project(homemind_pi4_core LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

add_executable(homemind-core src/core.cpp)
target_compile_features(homemind-core PRIVATE cxx_std_20)
target_compile_options(homemind-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(homemind-core PRIVATE
  PkgConfig::MOSQUITTO SQLite::SQLite3 nlohmann_json::nlohmann_json Threads::Threads)

add_executable(homemind-selftest src/selftest.cpp)
target_compile_features(homemind-selftest PRIVATE cxx_std_20)
target_compile_options(homemind-selftest PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(homemind-selftest PRIVATE
  PkgConfig::MOSQUITTO SQLite::SQLite3 nlohmann_json::nlohmann_json Threads::Threads)

add_executable(homemind-soaktest src/soaktest.cpp)
target_compile_features(homemind-soaktest PRIVATE cxx_std_20)
target_compile_options(homemind-soaktest PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(homemind-soaktest PRIVATE
  PkgConfig::MOSQUITTO SQLite::SQLite3 nlohmann_json::nlohmann_json Threads::Threads)

add_executable(homemind-simulator src/simulator.cpp)
target_compile_features(homemind-simulator PRIVATE cxx_std_20)
target_compile_options(homemind-simulator PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(homemind-simulator PRIVATE
  PkgConfig::MOSQUITTO nlohmann_json::nlohmann_json Threads::Threads)

foreach(target homemind-core homemind-selftest homemind-soaktest homemind-simulator)
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "\$ORIGIN/../lib;\$ORIGIN/lib"
    INSTALL_RPATH "\$ORIGIN/../lib;\$ORIGIN/lib")
endforeach()
