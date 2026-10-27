# homemind-vision: NODE-RPI5-02 figure presence service (cameras -> YOLOX-Nano -> zone -> FIGURE_PRESENCE).
if(NOT TARGET hm_vision)
  message(STATUS "homemind-vision: skipped (hm_vision not configured)")
  return()
endif()

add_executable(homemind-vision src/main.cpp src/settings.cpp)
# hm_cpp_httplib (vendored cpp-httplib) is defined by services/homemind-mobile-api: the home preview server.
target_link_libraries(homemind-vision PRIVATE hm_vision hm_common hm_cpp_httplib)
target_compile_options(homemind-vision PRIVATE ${HM_WARNINGS} -ffp-contract=off)
install(TARGETS homemind-vision RUNTIME DESTINATION bin)
