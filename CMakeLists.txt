cmake_minimum_required(VERSION 3.16)
project(qnu16 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

option(QNU_WITH_JACK "Build the JACK audio backend" ON)

find_package(Threads REQUIRED)

add_library(qnucore STATIC
  engine/src/params.cpp
  engine/src/topology.cpp
  engine/src/strip.cpp
  engine/src/fx.cpp
  engine/src/mixer.cpp
  engine/src/recorder.cpp
  engine/src/scene.cpp
  engine/src/controller.cpp
  engine/src/wav.cpp
  engine/src/server/websocket.cpp
  engine/src/server/server.cpp
  engine/src/backend/dummy_backend.cpp
  engine/src/backend/jack_backend.cpp
)
target_include_directories(qnucore PUBLIC engine/src)
target_link_libraries(qnucore PUBLIC Threads::Threads)

if(QNU_WITH_JACK)
  find_package(PkgConfig)
  if(PkgConfig_FOUND)
    pkg_check_modules(JACK jack)
  endif()
  if(JACK_FOUND)
    target_compile_definitions(qnucore PUBLIC QNU_HAVE_JACK)
    target_include_directories(qnucore PUBLIC ${JACK_INCLUDE_DIRS})
    target_link_libraries(qnucore PUBLIC ${JACK_LINK_LIBRARIES})
    message(STATUS "QNU16: JACK backend enabled")
  else()
    message(WARNING "QNU16: JACK not found (install libjack-jackd2-dev); only the dummy backend is available")
  endif()
endif()

add_executable(qnu16d engine/tools/qnu16d.cpp)
target_link_libraries(qnu16d PRIVATE qnucore)

add_executable(qnu16-render engine/tools/qnu16_render.cpp)
target_link_libraries(qnu16-render PRIVATE qnucore)

add_executable(qnu16-bench engine/tools/qnu16_bench.cpp)
target_link_libraries(qnu16-bench PRIVATE qnucore)

enable_testing()
add_executable(qnu16-tests tests/test_main.cpp tests/test_dsp.cpp tests/test_mixer.cpp tests/test_fx.cpp tests/test_qu.cpp tests/test_qu_routing.cpp tests/test_control.cpp)
target_link_libraries(qnu16-tests PRIVATE qnucore)
add_test(NAME qnu16-tests COMMAND qnu16-tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS qnu16d qnu16-render qnu16-bench RUNTIME DESTINATION bin)
install(DIRECTORY ui/ DESTINATION share/qnu16/ui)
