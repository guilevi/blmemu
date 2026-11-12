# The Braille Lite Millennium emulator.
#
#   tools/prepare_m32c.sh                 once: the RC8650's CPU (GDB's m32c simulator, patched by librc8650)
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#
# Windows from macOS or Linux: cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake_minimum_required(VERSION 3.16)
project(blmemu VERSION 0.1.0 LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()

set(RC8650_DIR "${CMAKE_SOURCE_DIR}/third_party/librc8650" CACHE PATH "librc8650's source")
set(M32C_DIR "${CMAKE_SOURCE_DIR}/third_party/m32c" CACHE PATH "the prepared m32c simulator (tools/prepare_m32c.sh)")
if(NOT EXISTS "${RC8650_DIR}/rc8650.c")
  message(FATAL_ERROR "no librc8650 in ${RC8650_DIR}: git submodule update --init, or -DRC8650_DIR=...")
endif()
if(NOT EXISTS "${M32C_DIR}/gen/m32c.c")
  message(FATAL_ERROR "no prepared m32c simulator in ${M32C_DIR}: run tools/prepare_m32c.sh first")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- librc8650 (GPL-3), its sources compiled in ----------------------------------------------------------------------
add_library(rc8650 STATIC
  ${RC8650_DIR}/rc8650.c ${RC8650_DIR}/stubs.c
  ${RC8650_DIR}/rcdict/rcdict.c ${RC8650_DIR}/rcdict/rcdict_regex.c
  ${M32C_DIR}/mem.c ${M32C_DIR}/reg.c ${M32C_DIR}/srcdest.c ${M32C_DIR}/misc.c ${M32C_DIR}/int.c
  ${M32C_DIR}/gen/r8c.c ${M32C_DIR}/gen/m32c.c)
target_include_directories(rc8650 PUBLIC ${RC8650_DIR} PRIVATE ${M32C_DIR} ${M32C_DIR}/gen ${RC8650_DIR}/rcdict)
# the simulator's sources predate the compilers' newer warnings; they are not this project's to clean
target_compile_options(rc8650 PRIVATE -w -Wno-implicit-function-declaration -Wno-incompatible-pointer-types
                                      -Wno-int-conversion)
if(WIN32)
  target_include_directories(rc8650 BEFORE PRIVATE ${RC8650_DIR}/win-compat)
  target_compile_options(rc8650 PRIVATE -include ${RC8650_DIR}/win-compat/rc8650-win.h)
endif()
if(NOT WIN32)
  target_link_libraries(rc8650 PUBLIC m)
endif()

# ---- MAME's Z180 core (BSD-3-Clause) ---------------------------------------------------------------------------------
add_library(z180 STATIC src/cpu/z180_mame.cpp src/cpu/z180_asci.cpp)
target_include_directories(z180 PUBLIC src/cpu)

# ---- the unit ----------------------------------------------------------------------------------------------------
add_library(blmcore STATIC
  src/blm_board.c src/sharp_flash.c src/blm_dt.c src/shell/fwstore.c src/shell/session.c src/shell/settings.c)
target_include_directories(blmcore PUBLIC src src/shell)
target_link_libraries(blmcore PUBLIC z180 rc8650)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(blmcore PUBLIC Threads::Threads)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(blmcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(MINGW)
  # one program, nothing beside it to install
  set(STATIC_RUNTIME -static -static-libgcc -static-libstdc++)
endif()

add_executable(blm_run tools/blm_run.c)
target_link_libraries(blm_run blmcore ${STATIC_RUNTIME})

# ---- the program a person runs -------------------------------------------------------------------------------------
if(WIN32)
  add_executable(blmemu WIN32 app/win/main_win.c app/win/audio_wasapi.c app/win/blmemu.rc)
  target_link_libraries(blmemu blmcore ole32 uuid comdlg32 user32 gdi32 advapi32 ${STATIC_RUNTIME})
  target_link_options(blmemu PRIVATE -municode)
else()
  add_executable(blmemu app/term/main_term.c app/term/keys_term.c)
  if(APPLE)
    target_sources(blmemu PRIVATE app/term/audio_mac.c)
    target_link_libraries(blmemu blmcore "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_sources(blmemu PRIVATE app/term/audio_alsa.c)
    target_include_directories(blmemu PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(blmemu blmcore ${ALSA_LIBRARIES})
  endif()
endif()
target_include_directories(blmemu PRIVATE app)
target_compile_definitions(blmemu PRIVATE BLMEMU_VERSION="${PROJECT_VERSION}")
