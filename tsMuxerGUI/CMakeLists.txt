# Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
# Apache License 2.0, see LICENSE and NOTICE.
cmake_minimum_required (VERSION 3.12)
project(tsMuxerGUI CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# No Qt Multimedia, on any platform. It was linked for the sound at the end of a mux only, and its
# deployment put Qt's FFmpeg plug-in and the FFmpeg libraries into the macOS and Linux packages,
# where tsMuxeR ships no FFmpeg. The sound is played by the system instead (myPlaySound in
# tsmuxerwindow.cpp), as it always was on Windows, where Qt is built from qtbase alone.
find_package(Qt6
  REQUIRED COMPONENTS Widgets LinguistTools)

# When cross-compiling for macOS, the pre-built Qt cmake config may reference
# the deprecated AGL framework which is absent from modern/osxcross SDKs.
# The reference lives in Qt's internal WrapOpenGL target (created by
# Qt6GuiDependencies.cmake) and propagates transitively through Qt6::Gui.
if(APPLE AND CMAKE_CROSSCOMPILING)
  # The pre-built Qt cmake config references the deprecated AGL framework
  # which is absent from modern/osxcross SDKs.  Strip it from Qt's internal
  # WrapOpenGL target (created by Qt6GuiDependencies.cmake).
  foreach(_qt_tgt WrapOpenGL::WrapOpenGL Qt6::Gui Qt6::Widgets)
    if(TARGET ${_qt_tgt})
      get_target_property(_libs ${_qt_tgt} INTERFACE_LINK_LIBRARIES)
      if(_libs)
        list(FILTER _libs EXCLUDE REGEX "AGL")
        set_target_properties(${_qt_tgt} PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
      endif()
    endif()
  endforeach()

  # The host rcc may have zstd support that the target Qt lacks, causing
  # unresolved _qt_resourceFeatureZstd symbols.  Disable zstd in rcc.
  set(CMAKE_AUTORCC_OPTIONS "--no-zstd")
endif()

set(tsmuxer_gui_libs Qt6::Widgets)

set(tsmuxer_ts_files
  translations/tsmuxergui_en.ts
  translations/tsmuxergui_ru.ts
  translations/tsmuxergui_fr.ts
  translations/tsmuxergui_zh.ts
  translations/tsmuxergui_de.ts
  translations/tsmuxergui_he.ts
  translations/tsmuxergui_es.ts
  translations/tsmuxergui_ja.ts
  translations/tsmuxergui_pt_BR.ts
  translations/tsmuxergui_pt_PT.ts
  translations/tsmuxergui_it.ts
  translations/tsmuxergui_zh_TW.ts
  translations/tsmuxergui_ko.ts
  translations/tsmuxergui_pl.ts
)

set(lang_qrc "translations.qrc")
configure_file(${lang_qrc} ${lang_qrc} COPYONLY)

qt6_add_translation(QM_FILES ${tsmuxer_ts_files})

# Make the embedded-resource (rcc) step depend on the compiled .qm, so a build always embeds the
# CURRENT translations. Without this, autorcc can embed a stale .qm and strings added since the last
# .qm build ship untranslated (this caused strings added from 2.12.0 onward to appear in English).
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${lang_qrc}
                            PROPERTIES OBJECT_DEPENDS "${QM_FILES}")

set(tsmuxer_gui_sources
  main.cpp
  tsmuxerwindow.cpp
  muxForm.cpp
  lang_codes.cpp
  tsmuxerwindow.ui
  muxForm.ui
  checkboxedheaderview.cpp
  discTitleDialog.cpp
  sampleDialog.cpp
  playerLauncher.cpp
  ac3CoreDialog.cpp
  images.qrc
  fontsettingstablemodel.cpp
  systemcolours.cpp
  ${QM_FILES}
  ${CMAKE_CURRENT_BINARY_DIR}/${lang_qrc}
)

if (WIN32)
  add_executable(tsMuxerGUI WIN32 ${tsmuxer_gui_sources} icon.rc)
elseif (APPLE)
  set(MACOSX_BUNDLE_ICON_FILE tsMuxerGUI.icns)
  set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.networkoptix.tsmuxergui")
  set(MACOSX_BUNDLE_BUNDLE_NAME "tsMuxerGUI")
  # Info.plist.in interpolates these three and nothing set them, so CMake substituted empty
  # strings and the shipped bundle carried no version at all: Finder's Get Info showed a blank
  # where every other application shows a number, while the window's own title bar had it.
  # Measured rather than assumed: CFBundleShortVersionString read out of a real built artefact
  # was the empty string.
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${VERSION_NUMBER}")
  set(MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_NUMBER}")
  set(MACOSX_BUNDLE_LONG_VERSION_STRING "${VERSION_NUMBER}")
  set_source_files_properties(tsMuxerGUI.icns PROPERTIES
         MACOSX_PACKAGE_LOCATION "Resources")
  add_executable(tsMuxerGUI MACOSX_BUNDLE ${tsmuxer_gui_sources} tsMuxerGUI.icns)
  set_target_properties(tsMuxerGUI PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  )
  # Copy the tsMuxeR CLI binary into the app bundle after building
  add_custom_command(TARGET tsMuxerGUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:tsmuxer>"
      "$<TARGET_BUNDLE_CONTENT_DIR:tsMuxerGUI>/MacOS/tsMuxeR"
    COMMENT "Copying tsMuxeR CLI into app bundle"
  )
# UNIX must be last here, as it's going to be true for APPLE and CYGWIN as well,
# but we want those to be handled by the code above.
elseif (UNIX)
  add_executable(tsMuxerGUI ${tsmuxer_gui_sources})
  install(TARGETS tsMuxerGUI DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES tsMuxerGUI.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES images/icon.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps/
          RENAME tsMuxerGUI.png)
endif()

target_link_libraries(tsMuxerGUI ${tsmuxer_gui_libs})

# PlaySound, used for the end-of-mux sound on Windows. See myPlaySound in tsmuxerwindow.cpp.
if(WIN32)
  target_link_libraries(tsMuxerGUI winmm)
endif()

# On macOS, ensure tsMuxeR CLI is built before the GUI (for the copy step)
if (APPLE)
  add_dependencies(tsMuxerGUI tsmuxer)
endif()
