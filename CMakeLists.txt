cmake_minimum_required(VERSION 3.10)
project(zssk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Typ budowania" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(zssk
  main.cpp
  src/archive.cpp
  src/codec.cpp
  src/commands.cpp
)
target_include_directories(zssk PRIVATE include)
target_link_libraries(zssk PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(zssk PRIVATE /W4 $<$<CONFIG:Release>:/O2>)
else()
  target_compile_options(zssk PRIVATE -Wall -Wextra
                         $<$<CONFIG:Release>:-O3>)
endif()

enable_testing()
add_test(NAME selftest COMMAND zssk selftest)