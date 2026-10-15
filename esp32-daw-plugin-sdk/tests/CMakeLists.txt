# SPDX-License-Identifier: MIT
add_executable(test_abi test_abi.c)
target_link_libraries(test_abi PRIVATE dawsdk_api)
add_test(NAME abi COMMAND test_abi)

add_executable(test_dsp test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE dawsdk_api m)
add_test(NAME dsp COMMAND test_dsp)

add_executable(test_plugins test_plugins.cpp ${CMAKE_SOURCE_DIR}/tools/validate.cpp)
target_include_directories(test_plugins PRIVATE ${CMAKE_SOURCE_DIR}/tools)
find_package(Threads REQUIRED)
target_link_libraries(test_plugins PRIVATE dawsdk_host m Threads::Threads)
add_test(NAME plugins COMMAND test_plugins)
set_target_properties(test_abi test_dsp test_plugins PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
