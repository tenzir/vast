cmake_minimum_required(VERSION 3.15...3.20 FATAL_ERROR)

project(
  vast_fuzz_setup
  VERSION 0.1.0
  DESCRIPTION "Code Intelligence fuzzing integration setup for VAST"
  HOMEPAGE_URL "https://github.com/tenzir/vast"
  LANGUAGES C CXX)

# CMake has no built-in way to report build flags for a specific target
# or source file. The `compile_commands.json` format has no support for
# linker flags at all. So as a workaround we look at the generated ninja
# files to extract the flags. We insist on Ninja because the flags are
# nicely grouped and easy to parse.
if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
  message (FATAL_ERROR "requires ninja generator")
endif ()

file(GLOB_RECURSE fuzz_targets_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_targets/*.cpp")

add_subdirectory(.. vast EXCLUDE_FROM_ALL)

foreach (fuzzer_file IN LISTS fuzz_targets_sources)
  get_filename_component(BASENAME "${fuzzer_file}" NAME_WE)

  # Add a dummy target to get the compiler/linker flags
  add_executable(${BASENAME} ${fuzzer_file})
  set_target_properties(${BASENAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  target_link_libraries(${BASENAME} vast::libvast)

  # Add a custom target to extract the args and 
  set(cc_pattern "build CMakeFiles/${BASENAME}.dir/fuzz_targets/${BASENAME}.cpp.o:")
  set(ld_pattern "build vast/bin/${BASENAME}:")
  add_custom_target (fuzzer_${BASENAME}_yaml
    ALL
    COMMAND ${CMAKE_SOURCE_DIR}/extract_flags.sh ${fuzzer_file}.yaml.in ${fuzzer_file}.yaml ${CMAKE_BINARY_DIR}/build.ninja '${cc_pattern}' '${ld_pattern}')
endforeach ()
