# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Allow managed deps (sepfy/libpeer, sepfy/srtp, sepfy/usrsctp) that still use
# cmake_minimum_required(<3.5) — CMake 4.x dropped that compat. Cache var covers the
# main configure; ENV covers the component-requirements child process project() spawns.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(ENV{CMAKE_POLICY_VERSION_MINIMUM} 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32p4-ipc)
