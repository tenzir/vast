# -- bundled dependencies ------------------------------------------------------

# TODO: VAST bundles robin-map in its aux directory, because we need at least
# 0.6.2, which is not widely available on package managers. We should
# investigate switching to a different map, e.g., from folly or abseil.

option(VAST_ENABLE_BUNDLED_ROBIN_MAP "Always use the bundled tsl-robin-map" OFF)
add_feature_info("VAST_ENABLE_BUNDLED_ROBIN_MAP" VAST_ENABLE_BUNDLED_ROBIN_MAP
                 "always use the tsl-robin-map.")
if (NOT VAST_ENABLE_BUNDLED_ROBIN_MAP)
  find_package(tsl-robin-map 0.6.2 QUIET)
  if (tsl-robin-map_FOUND)
    string(APPEND VAST_FIND_DEPENDENCY_LIST
           "\nfind_package(tsl-robin-map 0.6.2 REQUIRED)")
    dependency_summary("robin-map" tsl::robin_map "Dependencies")
  endif ()
endif ()
if (NOT tsl-robin-map_FOUND)
  if (NOT VAST_ENABLE_BUNDLED_ROBIN_MAP)
    message(
      STATUS
        "Cannot find installed tsl-robin-map; falling back to bundled version")
  endif ()
  add_subdirectory(aux/robin-map)
  dependency_summary("robin-map" "${CMAKE_CURRENT_SOURCE_DIR}/aux/robin-map"
                     "Dependencies")
  export(
    EXPORT tsl-robin-mapTargets
    FILE tsl-robin-mapTargets.cmake
    NAMESPACE tsl::)
  add_custom_target(
    tsl-robin-map-targets-link
    COMMAND
      ${CMAKE_COMMAND} -E create_symlink
      "${CMAKE_CURRENT_BINARY_DIR}/tsl-robin-mapTargets.cmake"
      "${CMAKE_BINARY_DIR}/tsl-robin-mapTargets.cmake"
    COMMENT "Linking tsl-robin-map targets")
  install(
    EXPORT tsl-robin-mapTargets
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vast"
    NAMESPACE tsl::)
  string(APPEND VAST_EXTRA_TARGETS_FILES
         "\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/tsl-robin-mapTargets.cmake\")")
  add_dependencies(robin_map tsl-robin-map-targets-link)
endif ()

# -- flattbuffers ---------------------------------------------------------------

file(GLOB flatbuffers_schemas CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/vast/fbs/*.fbs"
     "${CMAKE_CURRENT_SOURCE_DIR}/vast/fbs/legacy/*.fbs")
list(SORT flatbuffers_schemas)

VASTCompileFlatBuffers(
  TARGET libvast-fbs
  SCHEMAS ${flatbuffers_schemas}
  INCLUDE_DIRECTORY "vast/fbs")

# -- arrow ---------------------------------------------------------------------

find_package(Arrow 0.17 REQUIRED CONFIG)
mark_as_advanced(
  BROTLI_COMMON_LIBRARY
  BROTLI_DEC_LIBRARY
  BROTLI_ENC_LIBRARY
  LZ4_LIB
  Snappy_INCLUDE_DIR
  Snappy_LIB
  ZSTD_LIB)
string(
  APPEND
  VAST_FIND_DEPENDENCY_LIST
  "\nfind_package(Arrow REQUIRED CONFIG)"
  "\nmark_as_advanced("
  "\n BROTLI_COMMON_LIBRARY"
  "\n BROTLI_DEC_LIBRARY"
  "\n BROTLI_ENC_LIBRARY"
  "\n LZ4_LIB"
  "\n Snappy_INCLUDE_DIR"
  "\n Snappy_LIB"
  "\n ZSTD_LIB)")
if (BUILD_SHARED_LIBS)
  set(ARROW_LIBRARY arrow_shared)
else ()
  set(ARROW_LIBRARY arrow_static)
endif ()

# -- libsystemd ----------------------------------------------------------------

option(VAST_ENABLE_JOURNALD_LOGGING
       "Build with support for native journald log output" OFF)
add_feature_info("VAST_ENABLE_JOURNALD_LOGGING" VAST_ENABLE_JOURNALD_LOGGING
                 "build with support for native journald log output.")

# -- log level -----------------------------------------------------------------

# Choose a deafult log level based on build type.
if (CMAKE_BUILD_TYPE STREQUAL Release)
  set(VAST_LOG_LEVEL_DEFAULT "DEBUG")
elseif (CMAKE_BUILD_TYPE STREQUAL MinSizeRel)
  set(VAST_LOG_LEVEL_DEFAULT "DEBUG")
elseif (CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo)
  set(VAST_LOG_LEVEL_DEFAULT "DEBUG")
else ()
  set(VAST_LOG_LEVEL_DEFAULT "TRACE")
endif ()

# Make sure log level is defined and all-uppercase.
set(VAST_LOG_LEVEL_DOC "maximum log level available at runtime")
if (NOT VAST_LOG_LEVEL)
  set(VAST_LOG_LEVEL
      "${VAST_LOG_LEVEL_DEFAULT}"
      CACHE STRING "${VAST_LOG_LEVEL_DOC}")
elseif (NOT VAST_LOG_LEVEL STREQUAL "$CACHE{VAST_LOG_LEVEL}")
  # Override cached variable when re-running CMake.
  string(TOUPPER "${VAST_LOG_LEVEL}" VAST_LOG_LEVEL)
  set(VAST_LOG_LEVEL
      "${VAST_LOG_LEVEL}"
      CACHE STRING "${VAST_LOG_LEVEL_DOC}" FORCE)
endif ()

set(VAST_CAF_LOG_LEVEL
    "WARNING"
    CACHE STRING ${VAST_LOG_LEVEL_DOC})

# Raise an error for invalid log levels.
set(validLogLevels
    QUIET
    ERROR
    WARNING
    INFO
    VERBOSE
    DEBUG
    TRACE)
list(FIND validLogLevels "${VAST_LOG_LEVEL}" logLevelIndex)
if (logLevelIndex LESS 0)
  message(FATAL_ERROR "Invalid log level: \"${VAST_LOG_LEVEL}\"")
endif ()

# -- caf -----------------------------------------------------------------------

function (systemize_target tgt)
  message(STATUS "Treating ${tgt} as a system dependency")
  get_target_property(_includes ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
  set_property(
    TARGET ${tgt}
    APPEND
    PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_includes}")
endfunction ()

# TODO: Require CAF to be installed.

option(VAST_ENABLE_OPENSSL "Encrypt network communication" ON)
add_feature_info("VAST_ENABLE_OPENSSL" VAST_ENABLE_OPENSSL
                 "encrypt network communication.")

if (VAST_ENABLE_DEVELOPER_MODE
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/aux/caf/CMakeLists.txt"
    AND NOT CAF_ROOT_DIR)
  set(VAST_ENABLE_BUNDLED_CAF_DEFAULT ON)
else ()
  set(VAST_ENABLE_BUNDLED_CAF_DEFAULT OFF)
endif ()
option(VAST_ENABLE_BUNDLED_CAF "Use the CAF submodule"
       "${VAST_ENABLE_BUNDLED_CAF_DEFAULT}")
add_feature_info("VAST_ENABLE_BUNDLED_CAF" VAST_ENABLE_BUNDLED_CAF
                 "use the CAF submodule.")
if (NOT VAST_ENABLE_BUNDLED_CAF)
  # Try to find the required CAF components first...
  provide_find_module(CAF)
  find_package(
    CAF 0.17.6 EXACT
    COMPONENTS core io test
    REQUIRED)
  string(APPEND VAST_FIND_DEPENDENCY_LIST
         "\nfind_package(CAF 0.17.6 EXACT COMPONENTS core io test REQUIRED)")
  if (VAST_ENABLE_OPENSSL)
    find_package(
      CAF 0.17.6 EXACT
      COMPONENTS openssl
      REQUIRED)
    string(APPEND VAST_FIND_DEPENDENCY_LIST
           "\nfind_package(CAF 0.17.6 EXACT COMPONENTS openssl REQUIRED)")
  endif ()
else ()
  # Use bundled CAF.
  if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/aux/caf/CMakeLists.txt")
    message(
      FATAL_ERROR
        "CAF not found, either use -DCAF_ROOT_DIR=... or initialize the libvast/aux/caf submodule"
    )
  else ()
    set(VAST_ENABLE_BUNDLED_CAF ON)
    set(CAF_LOG_LEVEL ${VAST_CAF_LOG_LEVEL})
    set(CAF_NO_AUTO_LIBCPP ON)
    set(CAF_NO_OPENCL ON)
    set(CAF_NO_EXAMPLES ON)
    set(CAF_NO_UNIT_TESTS ON)
    set(CAF_NO_PYTHON ON)
    set(CAF_NO_TOOLS ON)
    set(CAF_NO_SUMMARY ON)
    if (NOT VAST_ENABLE_OPENSSL)
      set(CAF_NO_OPENSSL TRUE)
    endif ()
    if (BUILD_SHARED_LIBS)
      set(_linkage_suffix shared)
    else ()
      set(_linkage_suffix static)
      set(CAF_BUILD_STATIC ON)
      set(CAF_BUILD_STATIC_ONLY ON)
    endif ()
    add_subdirectory(aux/caf)
    systemize_target(libcaf_core_${_linkage_suffix})
    set_target_properties(libcaf_core_${_linkage_suffix} PROPERTIES EXPORT_NAME
                                                                    core)
    add_library(CAF::core ALIAS libcaf_core_${_linkage_suffix})
    target_compile_features(libcaf_core_${_linkage_suffix} INTERFACE cxx_std_17)
    target_compile_options(
      libcaf_core_${_linkage_suffix} PRIVATE -Wno-maybe-uninitialized
                                             -Wno-unknown-warning-option)
    systemize_target(libcaf_io_${_linkage_suffix})
    set_target_properties(libcaf_io_${_linkage_suffix} PROPERTIES EXPORT_NAME
                                                                  io)
    target_compile_options(
      libcaf_io_${_linkage_suffix} PRIVATE -Wno-maybe-uninitialized
                                           -Wno-unknown-warning-option)
    find_package(Threads REQUIRED)
    target_link_libraries(libcaf_io_${_linkage_suffix} Threads::Threads)
    add_library(CAF::io ALIAS libcaf_io_${_linkage_suffix})
    install(
      TARGETS libcaf_core_${_linkage_suffix} libcaf_io_${_linkage_suffix}
      EXPORT CAFTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if (VAST_ENABLE_OPENSSL)
      if (NOT TARGET libcaf_openssl_${_linkage_suffix})
        string(
          CONCAT err_message
                 "VAST_ENABLE_OPENSSL is active but the CAF OpenSSL module is"
                 " disabled. This is probably because the OpenSSL development"
                 " dependencies weren't found")
        message(FATAL_ERROR "${err_message}")
      endif ()
      target_include_directories(
        libcaf_openssl_${_linkage_suffix}
        PUBLIC
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/aux/caf/libcaf_openssl>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      systemize_target(libcaf_openssl_${_linkage_suffix})
      set_target_properties(libcaf_openssl_${_linkage_suffix}
                            PROPERTIES EXPORT_NAME openssl)
      add_library(CAF::openssl ALIAS libcaf_openssl_${_linkage_suffix})
      install(
        TARGETS libcaf_openssl_${_linkage_suffix}
        EXPORT CAFTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif ()
    add_library(libcaf_test INTERFACE)
    target_link_libraries(libcaf_test INTERFACE CAF::core)
    target_include_directories(
      libcaf_test
      INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/aux/caf/libcaf_test>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    systemize_target(libcaf_test)
    set_target_properties(libcaf_test PROPERTIES EXPORT_NAME test)
    add_library(CAF::test ALIAS libcaf_test)
    install(
      TARGETS libcaf_test
      EXPORT CAFTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(caf_dir ${CMAKE_CURRENT_SOURCE_DIR}/aux/caf)
    export(
      EXPORT CAFTargets
      FILE CAFTargets.cmake
      NAMESPACE CAF::)
    add_custom_target(
      caf-targets-link
      COMMAND
        ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_CURRENT_BINARY_DIR}/CAFTargets.cmake"
        "${CMAKE_BINARY_DIR}/CAFTargets.cmake"
      COMMENT "Linking CAF targets")
    install(
      EXPORT CAFTargets
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vast"
      NAMESPACE CAF::)
    mark_as_advanced(caf_build_header_path)
    string(APPEND VAST_EXTRA_TARGETS_FILES
           "\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/CAFTargets.cmake\")"
           "\nmark_as_advanced(caf_build_header_path)")
    set(CAF_FOUND true)
  endif ()
endif ()

if (VAST_ENABLE_RELOCATABLE_INSTALLATIONS
    AND BUILD_SHARED_LIBS
    AND CAF_LIBRARY_CORE)
  # Copy CAF libraries to installation directory
  get_filename_component(CAF_LIBDIR ${CAF_LIBRARY_CORE} PATH)
  file(GLOB CAF_INSTALLED_LIBRARIES "${CAF_LIBDIR}/libcaf*.so")
  install(FILES ${CAF_INSTALLED_LIBRARIES}
          DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif ()

# -- libvast -------------------------------------------------------------------

file(GLOB_RECURSE libvast_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/vast/*.hpp")
list(SORT libvast_sources)

add_library(libvast ${libvast_sources})
VASTTargetEnableTooling(libvast)
add_library(vast::libvast ALIAS libvast)

if ("${CMAKE_VERSION}" VERSION_GREATER_EQUAL "3.16")
  target_precompile_headers(libvast PUBLIC [["vast/fwd.hpp"]] <caf/fwd.hpp>)
endif ()

target_link_libraries(libvast PRIVATE libvast_internal)

# Use static versions of VAST_REGISTER_PLUGIN family of macros when inside the
# libvast library. This makes it possible to statically link a plugin against
# the vast::libvast target, or to write built-in plugins that are always loaded
# with libvast.
target_compile_definitions(libvast PRIVATE VAST_ENABLE_NATIVE_PLUGINS)

set_target_properties(libvast PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE
                                         ON)

set_target_properties(
  libvast
  PROPERTIES SOVERSION "${VAST_VERSION_MAJOR}"
             VERSION "${VAST_VERSION_MAJOR}.${VAST_VERSION_MINOR}"
             OUTPUT_NAME vast)

target_include_directories(
  libvast
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Make dlopen and dlclose available if we're building a shared library.
if (BUILD_SHARED_LIBS)
  target_link_libraries(libvast PUBLIC ${CMAKE_DL_LIBS})
endif ()

# Link against CAF.
target_link_libraries(libvast PUBLIC CAF::core CAF::io)
if (VAST_ENABLE_OPENSSL)
  target_link_libraries(libvast PUBLIC CAF::openssl)
endif ()
if (VAST_ENABLE_BUNDLED_CAF)
  # Figure out whether we point to a build directory or a prefix.
  if (NOT caf_dir)
    set(caf_dir ${CAF_INCLUDE_DIRS})
  endif ()
  add_dependencies(libvast caf-targets-link)
  dependency_summary("CAF" "${caf_dir}" "Dependencies")
else ()
  dependency_summary("CAF" CAF::core "Dependencies")
endif ()

# Link against libvast-fbs.
target_link_libraries(libvast PUBLIC libvast-fbs)

# Link against yaml-cpp.
find_package(yaml-cpp 0.6.2 REQUIRED)
target_link_libraries(libvast PRIVATE yaml-cpp)
dependency_summary("yaml-cpp" yaml-cpp "Dependencies")

# Link against libsystemd.
if (VAST_ENABLE_JOURNALD_LOGGING)
  find_package(systemd REQUIRED)
  target_link_libraries(libvast PRIVATE systemd::systemd)
  dependency_summary("libsystemd" systemd::systemd "Dependencies")
endif ()

# Link against fmt.
target_compile_definitions(
  libvast
  PUBLIC
    # Forbid unsafe duration_cast usage.
    FMT_SAFE_DURATION_CAST
    # Make fmt::internal an alias for fmt::detail. Remove when requiring fmt >= 7.
    FMT_USE_INTERNAL)
find_package(fmt 5.2.1 REQUIRED)
string(APPEND VAST_FIND_DEPENDENCY_LIST "\nfind_package(fmt 5.2.1 REQUIRED)")
target_link_libraries(libvast PUBLIC fmt::fmt)
dependency_summary("fmt" fmt::fmt "Dependencies")

target_compile_definitions(libvast PUBLIC SPDLOG_FMT_EXTERNAL)
find_package(spdlog 1.5.0 REQUIRED)
string(APPEND VAST_FIND_DEPENDENCY_LIST "\nfind_package(spdlog 1.5.0 REQUIRED)")
target_link_libraries(libvast PUBLIC spdlog::spdlog)
dependency_summary("spdlog" spdlog::spdlog "Dependencies")

# Link against simdjson.
find_package(
  simdjson
  REQUIRED
  # simdjson 0.8.0 exports it package configuration file under the wrong name,
  # so we look for "simdjson-targets.cmake" as well.
  CONFIGS
  "simdjson-config.cmake"
  "simdjson-targets.cmake")
string(APPEND VAST_FIND_DEPENDENCY_LIST
       "\nfind_package(simdjson REQUIRED CONFIGS "
       "\"simdjson-config.cmake\" \"simdjson-targets.cmake\")")
target_link_libraries(libvast PUBLIC simdjson::simdjson)
dependency_summary("simdjson" simdjson::simdjson "Dependencies")

# Link against robin-map.
target_link_libraries(libvast PUBLIC tsl::robin_map)
# This is broken for the bundled robin-map, so we special-case it above.
# dependency_summary("robin-map" tsl::robin_map "Dependencies")

# Link against Apache Arrow.
target_link_libraries(libvast PUBLIC ${ARROW_LIBRARY})
dependency_summary("Apache Arrow" ${ARROW_LIBRARY} "Dependencies")

# Link against a backtrace library.
option(VAST_ENABLE_BACKTRACE "Print a backtrace on unexpected termination" ON)
add_feature_info("VAST_ENABLE_BACKTRACE" VAST_ENABLE_BACKTRACE
                 "print a backtrace on unexpected termination.")
unset(VAST_ENABLE_LIBUNWIND)
unset(VAST_ENABLE_LIBBACKTRACE)
if (VAST_ENABLE_BACKTRACE)
  find_package(libunwind)
  if (libunwind_FOUND)
    target_link_libraries(libvast PRIVATE unwind::unwind)
    dependency_summary("libunwind" unwind::unwind "Dependencies")
    set(VAST_ENABLE_LIBUNWIND ON)
  else ()
    find_package(Backtrace)
    if (Backtrace_FOUND)
      target_include_directories(libvast
                                 PRIVATE ${Backtrace_INCLUDE_DIRECTORIES})
      target_link_libraries(libvast PRIVATE ${Backtrace_LIBRARIES})
      if ("${Backtrace_HEADER}" STREQUAL "backtrace.h")
        dependency_summary("libbacktrace" "${Backtrace_INCLUDE_DIR}"
                           "Dependencies")
        set(VAST_ENABLE_LIBBACKTRACE ON)
      elseif ("${Backtrace_HEADER}" STREQUAL "execinfo.h")
        dependency_summary("libexecinfo" "${Backtrace_INCLUDE_DIR}"
                           "Dependencies")
        set(VAST_ENABLE_LIBEXECINFO ON)
      else ()
        message(
          FATAL_ERROR "Found unexpected backtrace header ${Backtrace_HEADER}")
      endif ()
    endif ()
  endif ()
endif ()

# Configure options-dependent code files.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/detail/installdirs.cpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/src/detail/installdirs.cpp" @ONLY)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/vast/config.hpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/vast/config.hpp")

file(
  WRITE "${CMAKE_CURRENT_BINARY_DIR}/update-config.cmake"
  "\
  get_filename_component(CMAKE_MODULE_PATH
    \"${CMAKE_CURRENT_SOURCE_DIR}/../cmake\" ABSOLUTE)
  set(VAST_VERSION_TAG \"${VAST_VERSION_TAG_BACKUP}\")
  include(VASTVersion)
  configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp.in\"
                 \"${CMAKE_CURRENT_BINARY_DIR}/src/config.cpp\" @ONLY)")

# We generate config.cpp as a byproduct of a custom target so we can re-generate
# the version string and build tree hash on every build. Note that this must not
# be added to the ALL target. The dependency is inferred from the byproduct
# automatically.
add_custom_target(
  libvast_update_config
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/src/config.cpp"
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/update-config.cmake")

target_sources(
  libvast
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vast/config.hpp"
          "${CMAKE_CURRENT_BINARY_DIR}/src/config.cpp"
          "${CMAKE_CURRENT_BINARY_DIR}/src/detail/installdirs.cpp")

# TODO: Should we move the bundled schemas to libvast?
if (TARGET vast-schema)
  add_dependencies(libvast vast-schema)
endif ()

# Install libvast in PREFIX/lib and headers in PREFIX/include/vast.
install(
  TARGETS libvast
  EXPORT VASTTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  DIRECTORY vast
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.hpp")

# Install generated config headers.
install(
  DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vast"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.hpp")

add_subdirectory(test)

set(VAST_FIND_DEPENDENCY_LIST
    "${VAST_FIND_DEPENDENCY_LIST}"
    PARENT_SCOPE)
set(VAST_EXTRA_TARGETS_FILES
    "${VAST_EXTRA_TARGETS_FILES}"
    PARENT_SCOPE)
