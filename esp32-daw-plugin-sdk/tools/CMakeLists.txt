# SPDX-License-Identifier: MIT
add_executable(dawplug dawplug.cpp validate.cpp)
target_link_libraries(dawplug PRIVATE dawsdk_host m)
target_compile_options(dawplug PRIVATE -Wall -Wextra)
set_target_properties(dawplug PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
