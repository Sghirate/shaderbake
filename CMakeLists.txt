cmake_minimum_required(VERSION 3.10)
project(shaderbake LANGUAGES C)

# Set output directory to /bin in standalone mode
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY $<1:${CMAKE_SOURCE_DIR}/bin>)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_SOURCE_DIR}/bin>)
endif()

# Dependency Handling:
# SDL3::SDL3 and SDL3_shadercross::SDL3_shadercross-static can either be provided
# from 'the outside' if this is included via add_subdirectory
# Otherwise this will attempt to find a local installation of the dependencies or
# download them from GitHub.
include(FetchContent)
function(include_dependency libName gitURL gitTag)
  # setup the declare
  FetchContent_Declare(        ${libName}
        GIT_REPOSITORY         ${gitURL}
        GIT_TAG                ${gitTag}
        GIT_SHALLOW            TRUE
        GIT_PROGRESS           TRUE
        GIT_SUBMODULES_RECURSE TRUE
  )
  FetchContent_MakeAvailable(${libName})
endfunction()
if(NOT TARGET SDL3::SDL3)
  find_package(SDL3)
  if (NOT SDL3_FOUND)
    message(STATUS "Getting SDL3 from Github")
    include_dependency(SDL3 https://github.com/libsdl-org/SDL.git release-3.4.0)
  else()
    message(STATUS "Using local SDL3")
  endif()
endif()
if(NOT TARGET SDL3_shadercross::SDL3_shadercross-static)
  message(STATUS "Getting SDL_shadercross from Github")
  set(SDLSHADERCROSS_VENDORED ON CACHE BOOL "Use vendored dependencies" FORCE)
  include_dependency(SDL_shadercross https://github.com/libsdl-org/SDL_shadercross.git main)
endif()

include(CMakeDependentOption)
cmake_dependent_option(SHADERBAKE_LEAKCHECK "Check shaderbake for memory leaks" OFF "TARGET SDL3::SDL3_test" OFF)

# ShaderBake set up
add_executable(shaderbake main.c)
target_compile_options(shaderbake PUBLIC
  -Wall -Werror -Wno-error=maybe-uninitialized -Wno-unknown-pragmas
  $<$<CONFIG:DEBUG>:-O0>
  $<$<CONFIG:DEBUG>:-g>
  $<$<CONFIG:RELEASE>:-Ofast>
)
target_link_libraries(shaderbake PRIVATE
  SDL3::SDL3
  SDL3_shadercross::SDL3_shadercross-static
)
if(SHADERBAKE_LEAKCHECK)
  target_link_libraries(shaderbake PRIVATE SDL3::SDL3_test)
  target_compile_definitions(shaderbake PRIVATE LEAKCHECK)
endif()
