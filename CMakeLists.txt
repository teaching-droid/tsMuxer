# Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
# Apache License 2.0, see LICENSE and NOTICE.
cmake_minimum_required (VERSION 3.12)
project (tsmuxer_main CXX)

# A build without a build type is compiled without optimisation. The Linux release script gave
# none, so the Linux programs ran slower than the Windows and macOS ones, built as Release, and
# slower than the builds the tests run. Release is the default now; a type that is given wins.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_EXTENSIONS FALSE)

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  RESULT_VARIABLE GIT_REV_SHORT_RV
  OUTPUT_VARIABLE GIT_REV_SHORT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(TSMUXER_RELEASE FALSE)
set(VERSION_NUMBER "3.1.0")
if(${TSMUXER_RELEASE})
  set(TSMUXER_VERSION ${VERSION_NUMBER})
elseif(${GIT_REV_SHORT_RV} EQUAL 0)
  set(TSMUXER_VERSION "${VERSION_NUMBER} (git-${GIT_REV_SHORT})")
else()
  set(TSMUXER_VERSION "${VERSION_NUMBER}-dev")
endif()
add_definitions("-DTSMUXER_VERSION=\"${TSMUXER_VERSION}\"")

# The commit above is read at CONFIGURE time and baked into the compile flags, so building again
# in an already configured directory keeps printing whatever commit it was configured with. A
# local build here reported a commit 24 behind the one it actually contained, which matters
# because that line is what people quote in a bug report, and it was believed once already.
# Watching the files git rewrites when HEAD moves makes cmake re-run on the next build, which
# costs a fraction of a second on an existing build directory. Nothing is watched when git is
# absent, so a source tarball builds exactly as before.
set(TSMUXER_GIT_DIR "${CMAKE_SOURCE_DIR}/.git")
if(EXISTS "${TSMUXER_GIT_DIR}" AND NOT IS_DIRECTORY "${TSMUXER_GIT_DIR}")
  # In a worktree or a submodule .git is a file naming the real directory.
  file(READ "${TSMUXER_GIT_DIR}" TSMUXER_GIT_FILE)
  if(TSMUXER_GIT_FILE MATCHES "gitdir: *([^\r\n]+)")
    get_filename_component(TSMUXER_GIT_DIR "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
  endif()
endif()
if(IS_DIRECTORY "${TSMUXER_GIT_DIR}")
  # A worktree's own directory holds only its HEAD and its log; the branch refs and packed-refs are in the
  # repository's common directory, which "commondir" names. Watching the branch ref in the worktree's own
  # directory watched a file that does not exist there, so a build in a worktree kept the commit it was first
  # configured with: one printed a commit two merges older than the one it was built from.
  set(TSMUXER_GIT_COMMON "${TSMUXER_GIT_DIR}")
  if(EXISTS "${TSMUXER_GIT_DIR}/commondir")
    file(READ "${TSMUXER_GIT_DIR}/commondir" TSMUXER_GIT_COMMONDIR)
    string(STRIP "${TSMUXER_GIT_COMMONDIR}" TSMUXER_GIT_COMMONDIR)
    get_filename_component(TSMUXER_GIT_COMMON "${TSMUXER_GIT_COMMONDIR}" ABSOLUTE BASE_DIR "${TSMUXER_GIT_DIR}")
  endif()
  # HEAD covers a checkout and a detached head; the branch ref covers a commit on the current
  # branch, which does NOT rewrite HEAD; packed-refs covers a tree whose refs are packed; the log of HEAD is
  # written by every commit, merge and checkout, also while the branch ref is packed and has no file of its own.
  set(TSMUXER_GIT_WATCH "${TSMUXER_GIT_DIR}/HEAD" "${TSMUXER_GIT_DIR}/logs/HEAD" "${TSMUXER_GIT_COMMON}/packed-refs")
  if(EXISTS "${TSMUXER_GIT_DIR}/HEAD")
    file(READ "${TSMUXER_GIT_DIR}/HEAD" TSMUXER_GIT_HEAD)
    if(TSMUXER_GIT_HEAD MATCHES "ref: *([^\r\n]+)")
      list(APPEND TSMUXER_GIT_WATCH "${TSMUXER_GIT_COMMON}/${CMAKE_MATCH_1}")
    endif()
  endif()
  foreach(watched IN LISTS TSMUXER_GIT_WATCH)
    if(EXISTS "${watched}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${watched}")
    endif()
  endforeach()
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  include(GNUInstallDirs)
  mark_as_advanced(CLEAR
    CMAKE_INSTALL_BINDIR
    CMAKE_INSTALL_DATADIR
  )
else()
  set(CMAKE_INSTALL_BINDIR bin)
endif()

if(MSVC)
  add_compile_options("/utf-8")
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
  add_definitions(-DUNICODE -D_UNICODE)
endif()

if(TSMUXER_STATIC_BUILD AND UNIX)
  list(INSERT CMAKE_FIND_LIBRARY_SUFFIXES 0 .a)
  list(REMOVE_DUPLICATES CMAKE_FIND_LIBRARY_SUFFIXES)
endif()

set(TSMUXER_COVERAGE FALSE CACHE BOOL "Enable collecting coverage information")
if(${TSMUXER_COVERAGE})
  if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    add_compile_options("--coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
  elseif(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
    add_compile_options("-fprofile-instr-generate" "-fcoverage-mapping")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate")
  else()
    message(FATAL_ERROR "Coverage not supported for compiler ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

add_subdirectory(libmediation)
add_subdirectory(tsMuxer)

# Qt's Gui target passes OpenGL on to every program that links it. With the GLVND libraries CMake prefers,
# that named libOpenGL.so.0 and libGLX.so.0 in tsMuxerGUI, which calls no OpenGL function itself, and the
# window did not start on a system without the separate libOpenGL package (libopengl0 on Debian and Ubuntu,
# libglvnd-opengl on Fedora). libGL.so.1, which Qt's Gui library needs anyway, is enough. Qt only sets its
# own preference when none is set, so this has to come before Qt is looked for.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(OpenGL_GL_PREFERENCE LEGACY CACHE STRING "OpenGL libraries to link on Linux: LEGACY or GLVND")
endif()

# Auto-detect Qt6 and enable the GUI by default if found
if(NOT DEFINED TSMUXER_GUI)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    set(TSMUXER_GUI TRUE)
    message(STATUS "Qt6 found - GUI will be built (disable with -DTSMUXER_GUI=OFF)")
  else()
    set(TSMUXER_GUI FALSE)
    message(STATUS "Qt6 not found - GUI will not be built (install Qt6 or set -DTSMUXER_GUI=ON)")
  endif()
endif()

if(TSMUXER_GUI)
  add_subdirectory(tsMuxerGUI)
endif()
