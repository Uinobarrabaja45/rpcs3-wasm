# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-src")
  file(MAKE_DIRECTORY "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-src")
endif()
file(MAKE_DIRECTORY
  "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-build"
  "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix"
  "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix/tmp"
  "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix/src/absl-populate-stamp"
  "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix/src"
  "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix/src/absl-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix/src/absl-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bank23232525/Documents/works/emu/rpcs3/build_cmake/build/root-wasm/_deps/absl-subbuild/absl-populate-prefix/src/absl-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
