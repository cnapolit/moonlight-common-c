# Tests of the Vibepollo protocol extensions. A standalone project so the library's own
# CMakeLists.txt stays as upstream has it:
#   cmake -S tests/vibepollo -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.10)
project(moonlight-common-c-vibepollo-tests C)

set(CMAKE_C_STANDARD 11)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. moonlight-common-c)

enable_testing()

function(vibepollo_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src ${CMAKE_CURRENT_SOURCE_DIR}/../../enet/include)
  target_link_libraries(${name} PRIVATE moonlight-common-c)
  if (NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

vibepollo_test(test_callbacks_layout)
vibepollo_test(test_controller_haptics)
vibepollo_test(test_controller_speaker)
vibepollo_test(test_pyrowave_frames)
vibepollo_test(test_steam_controller)
