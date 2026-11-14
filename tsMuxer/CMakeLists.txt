# Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
# Apache License 2.0, see LICENSE and NOTICE.
cmake_minimum_required (VERSION 3.10)
project (tsmuxer LANGUAGES CXX)

add_executable (tsmuxer
  aac.cpp
  aacStreamReader.cpp
  av1.cpp
  av1StreamReader.cpp
  vpxStreamReader.cpp
  vorbisStreamReader.cpp
  alacStreamReader.cpp
  wavpackStreamReader.cpp
  wavpackFile.cpp
  mkvCopyStreamReader.cpp
  videoCopyStreamReader.cpp
  bdSsifImport.cpp
  abstractDemuxer.cpp
  abstractMuxer.cpp
  ac3Codec.cpp
  ac3StreamReader.cpp
  assFile.cpp
  diagnosticReport.cpp
  streamReport.cpp
  sampleCutter.cpp
  batchRunner.cpp
  pgsForcedFilter.cpp
  oggFile.cpp
  assRender.cpp
  avCodecs.cpp
  bitStream.cpp
  bitmapSubtitles.cpp
  blurayHelper.cpp
  chapterFile.cpp
  bufferedFileReader.cpp
  bufferedFileWriter.cpp
  bufferedReader.cpp
  bufferedReaderManager.cpp
  combinedH264Demuxer.cpp
  convertUTF.cpp
  discMeta.cpp
  discTitles.cpp
  doviLib.cpp
  dvdTitles.cpp
  encryptedInput.cpp
  dtsStreamReader.cpp
  dvbSubStreamReader.cpp
  flacStreamReader.cpp
  h264StreamReader.cpp
  hevc.cpp
  audioConvert.cpp
  dvLayerPicture.cpp
  jsonListing.cpp
  hevcDolbyVisionFilter.cpp
  hevcStreamReader.cpp
  ioContextDemuxer.cpp
  iso_writer.cpp
  latm.cpp
  lpcmStreamReader.cpp
  main.cpp
  matroskaDemuxer.cpp
  matroskaMuxer.cpp
  matroskaParser.cpp
  metaDemuxer.cpp
  mlpCodec.cpp
  mlpStreamReader.cpp
  trueHDAC3MergeReader.cpp
  movDemuxer.cpp
  mp3Codec.cpp
  h264PictureEnd.cpp
  hevcPictureEnd.cpp
  mpeg2PictureEnd.cpp
  mpeg2StreamReader.cpp
  mpegAudioStreamReader.cpp
  mpegStreamReader.cpp
  mpegVideo.cpp
  muxerManager.cpp
  nalUnits.cpp
  opusStreamReader.cpp
  pesPacket.cpp
  programStreamDemuxer.cpp
  pgsStreamReader.cpp
  simplePacketizerReader.cpp
  singleFileMuxer.cpp
  srtStreamReader.cpp
  textSubtitles.cpp
  textSubtitlesRender.cpp
  textstParser.cpp
  tsDemuxer.cpp
  tsMuxer.cpp
  tstdCheck.cpp
  tsPacket.cpp
  utf8Converter.cpp
  vc1Parser.cpp
  vc1StreamReader.cpp
  vod_common.cpp
  vvc.cpp
  vvcStreamReader.cpp
  wave.cpp
)

if(TSMUXER_STATIC_BUILD)
  if(MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_compile_options(tsmuxer "/MTd")
    else()
      target_compile_options(tsmuxer "/MT")
    endif()
  else()
    # static linking isn't supported on Mac
    if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
      set (CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")
    endif()
  endif()
  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
endif()

function(pkg_check_modules_with_static prefix req_or_opt package)
  pkg_check_modules (${prefix} ${req_or_opt} ${package})
  if (TSMUXER_STATIC_BUILD)
    set(static_libs "${${prefix}_STATIC_LIBRARIES}")
    # for osxcross if we want a mostly static build we need to append "-static" to the libraries
    if(DEFINED OSXCROSS_SDK)
      list(TRANSFORM static_libs APPEND "-static")
    endif()
    set(${prefix}_LIBRARIES "${static_libs}" CACHE INTERNAL "")
  endif()
endfunction()

find_package (Threads REQUIRED)
find_package (PkgConfig)

if (${PkgConfig_FOUND} AND NOT ${WITHOUT_PKGCONFIG})
  pkg_check_modules_with_static (ZLIB REQUIRED zlib)
  if (NOT WIN32)
    pkg_check_modules_with_static (FREETYPE REQUIRED freetype2)
  endif()
else()
  find_package(ZLIB REQUIRED)
  if (NOT WIN32)
    find_package(Freetype REQUIRED)
  endif()
endif()

target_include_directories(tsmuxer PRIVATE
  "${PROJECT_SOURCE_DIR}/../libmediation"
  ${ZLIB_INCLUDE_DIRS}
)

# this part looks messy as it is working around a bug in pthread when static linking
SET(THREADSLIB Threads::Threads)
if(TSMUXER_STATIC_BUILD)
  if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    message("-- Static Linux build, will link whole pthread!")
    set_target_properties(tsmuxer PROPERTIES LINK_SEARCH_START_STATIC 1)
    set_target_properties(tsmuxer PROPERTIES LINK_SEARCH_END_STATIC 1)
    SET(THREADSLIB -pthread -Wl,--whole-archive -lpthread -Wl,--no-whole-archive)
  endif()
endif()

if (WIN32)
  target_sources(tsmuxer PRIVATE osdep/textSubtitlesRenderWin32.cpp)
  target_link_libraries(tsmuxer gdiplus)
  # THE STACK OF THE WINDOWS PROGRAM. A Windows program gets a stack of 1 MB unless it asks for more; on Linux and
  # macOS the main thread has 8 MB. The reader of a transport stream holds tables of about 140 KB, and such readers
  # are also made on the stack, one inside the call of another, where a file is looked at before it is muxed. A
  # job of a video and a sound track of a transport stream was measured at 770 to 900 KB of stack: one table more in
  # that reader, and the program ended at once without a word (exit 127). 8 MB are asked for, as elsewhere: address
  # space that is reserved, and memory only as far as it is used.
  if(MSVC)
    target_link_options(tsmuxer PRIVATE "/STACK:8388608")
  else()
    target_link_options(tsmuxer PRIVATE "-Wl,--stack,8388608")
  endif()
else()
  target_sources(tsmuxer PRIVATE osdep/textSubtitlesRenderFT.cpp)
  # on osxcross use the static freetype library explicitly
  if(DEFINED OSXCROSS_SDK)
    list(TRANSFORM FREETYPE_LDFLAGS REPLACE "(-lfreetype)" "-lfreetype-static")
  endif()
  target_link_libraries(tsmuxer ${FREETYPE_LIBRARIES} ${FREETYPE_LDFLAGS})
  target_include_directories(tsmuxer PRIVATE ${FREETYPE_INCLUDE_DIRS})
endif()

# libdovi is loaded by name at run time, never linked, so only the loader itself is needed here
# (dlopen lives in libdl on some systems; CMAKE_DL_LIBS is empty where it does not).
target_link_libraries(tsmuxer mediation ${THREADSLIB} ${ZLIB_LIBRARIES} ${CMAKE_DL_LIBS})

install (TARGETS tsmuxer DESTINATION ${CMAKE_INSTALL_BINDIR})
