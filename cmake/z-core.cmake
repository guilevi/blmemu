# z-core's static library: by default the one its release publishes for this platform (no Rust needed); with
# -DZCORE_PREBUILT=OFF, Cargo builds it from ZCORE_DIR.  The release and the submodule are the same z-core version.
option(ZCORE_PREBUILT "Link z-core's released static library instead of building it with Rust" ON)
set(ZCORE_DIR "${CMAKE_SOURCE_DIR}/third_party/z-core" CACHE PATH "z-core's source")
if(ZCORE_PREBUILT)
  set(ZCORE_VERSION 0.1.1)
  if(WIN32)
    set(zcore_platform x86_64-windows-gnu)
    set(zcore_sha256 c06b4a6bae9deba07f801cc60eddf6316c97915f4903f947936130b31a1f835c)
  elseif(APPLE)
    set(zcore_platform universal-apple-darwin)
    set(zcore_sha256 fe76ff688d396ce6c45e87b5fd320d9ef0a839e81a95aa6cd40c93ab36723ed9)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(zcore_platform x86_64-linux-gnu)
    set(zcore_sha256 690844cb72e0fee43b6b53baebe46b30a4261ae8a34b39f4e65226d6672052a6)
  else()
    message(FATAL_ERROR "z-core publishes no library for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}: "
      "configure with -DZCORE_PREBUILT=OFF to build it with Rust")
  endif()
  set(zcore_name "z180-c-${ZCORE_VERSION}-${zcore_platform}")
  set(zcore_tarball "${CMAKE_CURRENT_BINARY_DIR}/${zcore_name}.tar.gz")
  set(zcore_pkg "${CMAKE_CURRENT_BINARY_DIR}/${zcore_name}")
  if(NOT EXISTS "${zcore_pkg}/lib/libz180.a")
    file(DOWNLOAD "https://github.com/ctoth/z-core/releases/download/v${ZCORE_VERSION}/${zcore_name}.tar.gz"
      "${zcore_tarball}" EXPECTED_HASH SHA256=${zcore_sha256} TLS_VERIFY ON STATUS zcore_status)
    list(GET zcore_status 0 zcore_code)
    if(NOT zcore_code EQUAL 0)
      list(GET zcore_status 1 zcore_message)
      message(FATAL_ERROR "could not download ${zcore_name}.tar.gz: ${zcore_message}\n"
        "Place that file in ${CMAKE_CURRENT_BINARY_DIR}, or configure with -DZCORE_PREBUILT=OFF")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${zcore_tarball}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" RESULT_VARIABLE zcore_code)
    if(NOT zcore_code EQUAL 0 OR NOT EXISTS "${zcore_pkg}/lib/libz180.a")
      message(FATAL_ERROR "could not unpack ${zcore_tarball}")
    endif()
  endif()
  file(READ "${zcore_pkg}/native-static-libs.txt" zcore_native)
  separate_arguments(zcore_native UNIX_COMMAND "${zcore_native}")
  set(zcore_links)
  set(zcore_framework)
  foreach(item IN LISTS zcore_native)       # "-framework X" stays one link item
    if(zcore_framework)
      list(APPEND zcore_links "-framework ${item}")
      set(zcore_framework)
    elseif(item STREQUAL "-framework")
      set(zcore_framework 1)
    else()
      list(APPEND zcore_links "${item}")
    endif()
  endforeach()
  add_library(zcore_ffi STATIC IMPORTED)
  set_target_properties(zcore_ffi PROPERTIES IMPORTED_LOCATION "${zcore_pkg}/lib/libz180.a"
    INTERFACE_INCLUDE_DIRECTORIES "${zcore_pkg}/include"
    INTERFACE_COMPILE_DEFINITIONS Z180_STATIC
    INTERFACE_LINK_LIBRARIES "${zcore_links}")
  return()
endif()
if(NOT EXISTS "${ZCORE_DIR}/crates/z180-c/include/z180.h")
  message(FATAL_ERROR "no z-core in ${ZCORE_DIR}: git submodule update --init")
endif()
find_program(CARGO_EXECUTABLE cargo)
find_program(RUSTC_EXECUTABLE rustc)
if(NOT CARGO_EXECUTABLE OR NOT RUSTC_EXECUTABLE)
  message(FATAL_ERROR "Rust and Cargo are required to build z-core")
endif()
if(WIN32)
  set(zcore_targets x86_64-pc-windows-gnu)
elseif(APPLE)
  set(zcore_archs ${CMAKE_OSX_ARCHITECTURES})
  if(NOT zcore_archs)
    set(zcore_archs ${CMAKE_SYSTEM_PROCESSOR})
  endif()
  foreach(arch IN LISTS zcore_archs)
    if(arch STREQUAL "arm64")
      list(APPEND zcore_targets aarch64-apple-darwin)
    elseif(arch STREQUAL "x86_64")
      list(APPEND zcore_targets x86_64-apple-darwin)
    else()
      message(FATAL_ERROR "unsupported macOS architecture: ${arch}")
    endif()
  endforeach()
else()
  execute_process(COMMAND "${RUSTC_EXECUTABLE}" -vV OUTPUT_VARIABLE rust_version RESULT_VARIABLE rust_result)
  if(NOT rust_result EQUAL 0)
    message(FATAL_ERROR "rustc could not identify the native build target")
  endif()
  string(REGEX MATCH "host: ([^\r\n]+)" rust_host "${rust_version}")
  set(zcore_targets "${CMAKE_MATCH_1}")
endif()
string(REPLACE ";" "|" zcore_targets_arg "${zcore_targets}")
file(GLOB_RECURSE zcore_sources CONFIGURE_DEPENDS
  "${ZCORE_DIR}/crates/z180-core/src/*.rs" "${ZCORE_DIR}/crates/z180-c/src/*.rs")
set(zcore_archive "${CMAKE_CURRENT_BINARY_DIR}/rust/libz180.a")
set(zcore_native_libs "${CMAKE_CURRENT_BINARY_DIR}/rust/native-libs.rsp")
add_custom_command(OUTPUT "${zcore_archive}" "${zcore_native_libs}"
  COMMAND "${CMAKE_COMMAND}"
    "-DZCORE_DIR=${ZCORE_DIR}" "-DCARGO=${CARGO_EXECUTABLE}"
    "-DRUST_TARGETS=${zcore_targets_arg}" "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust"
    "-DC_COMPILER=${CMAKE_C_COMPILER}" "-DDEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-z-core.cmake"
  DEPENDS ${zcore_sources} "${ZCORE_DIR}/Cargo.toml" "${ZCORE_DIR}/Cargo.lock"
    "${ZCORE_DIR}/crates/z180-core/Cargo.toml" "${ZCORE_DIR}/crates/z180-c/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-z-core.cmake"
  VERBATIM)
add_custom_target(zcore_build DEPENDS "${zcore_archive}" "${zcore_native_libs}")
add_library(zcore_ffi STATIC IMPORTED)
set_target_properties(zcore_ffi PROPERTIES IMPORTED_LOCATION "${zcore_archive}"
  INTERFACE_INCLUDE_DIRECTORIES "${ZCORE_DIR}/crates/z180-c/include"
  INTERFACE_COMPILE_DEFINITIONS Z180_STATIC
  INTERFACE_LINK_LIBRARIES "-Wl,@${zcore_native_libs}")
add_dependencies(zcore_ffi zcore_build)
