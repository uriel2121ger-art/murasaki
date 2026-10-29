# Qt Core + D-Bus only (plus Qt Qml for the property model when available), so the GUI's logic is built and tested on CI whether or not the GUI stack is there.
add_library(murasaki_app_logic STATIC
  daemonclient.cpp daemonclient.h
  daemonclientautomation.cpp
  daemonclientdemos.cpp
  daemonclientscreen.cpp
  dbusbackend.cpp dbusbackend.h
  libraryfiltermodel.cpp libraryfiltermodel.h
  librarymodel.cpp librarymodel.h
  overlayclient.cpp overlayclient.h
  surfaceclient.cpp surfaceclient.h
  systemcontrol.cpp systemcontrol.h
  usenowflow.cpp usenowflow.h
  workshopstatus.cpp workshopstatus.h)
target_include_directories(murasaki_app_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_app_logic PUBLIC murasaki_core murasaki_plasma murasaki_render_logic murasaki_shortcut Qt6::Core Qt6::DBus)
# PropertyModel evaluates property conditions with QJSEngine, so it needs Qt Qml (no GUI stack). Qml is optional
# here: without it the library simply lacks the property model (the GUI executable requires Qml anyway).
find_package(Qt6 6.8 QUIET COMPONENTS Qml)
if(Qt6Qml_FOUND)
  target_sources(murasaki_app_logic PRIVATE propertymodel.cpp propertymodel.h)
  target_link_libraries(murasaki_app_logic PUBLIC Qt6::Qml)
endif()

if(MURASAKI_APP)
  qt_add_executable(murasaki main.cpp)
  # Kirigami is imported by Main.qml at run time (org.kde.kirigami); it is not a link dependency.
  qt_add_qml_module(murasaki
    URI Murasaki.App
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
      qml/Main.qml
      qml/LibraryPage.qml
      qml/ItemCard.qml
      qml/ItemPage.qml
      qml/AddToPlaylist.qml
      qml/DemoCard.qml
      qml/DemoMessage.qml
      qml/DemoRemoveDialog.qml
      qml/DemoSupport.qml
      qml/EngineFailureMessage.qml
      qml/ImportDialog.qml
      qml/PetPage.qml
      qml/PetSound.qml
      qml/PetStateRow.qml
      qml/PetStates.qml
      qml/PlaylistNameDialog.qml
      qml/PlaylistPage.qml
      qml/PlaylistsPage.qml
      qml/ProfilesPage.qml
      qml/TimelineDialog.qml
      qml/TimelinesPage.qml
      qml/ReferenceCaptureDialog.qml
      qml/RegionPicker.qml
      qml/ScreenTriggerDialog.qml
      qml/ScreenWatchBanner.qml
      qml/ScreenDemoDialog.qml
      qml/TriggerDialog.qml
      qml/TriggersPage.qml
      qml/AutomationsPage.qml
      qml/AutomationEditor.qml
      qml/AutomationRow.qml
      qml/AutomationText.qml
      qml/AppPicker.qml
      qml/PauseAppsDialog.qml
      qml/TargetState.qml
      qml/PropertyEditor.qml
      qml/WorkshopPage.qml
      qml/WorkshopProfile.qml
      qml/ScreensPage.qml
      qml/SettingsPage.qml
      qml/Hint.qml
      qml/settings/GeneralSettings.qml
      qml/settings/PerformanceSettings.qml
      qml/settings/PauseSettings.qml
      qml/settings/PetsSettings.qml
      qml/settings/AutomationSettings.qml
      qml/settings/ImportSettings.qml
      qml/settings/SteamSettings.qml
      qml/settings/PrivacySettings.qml
      qml/settings/AboutSettings.qml
      qml/SteamCmdDialog.qml
      qml/props/SliderProp.qml
      qml/props/BoolProp.qml
      qml/props/ColorProp.qml
      qml/props/ComboProp.qml
      qml/props/TextProp.qml
      qml/props/FileProp.qml
      qml/props/ShortcutProp.qml)
  target_link_libraries(murasaki PRIVATE
    murasaki_app_logic murasaki_core
    Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::WebEngineQuick)

  # Translations. qt_add_translations needs Qt6LinguistTools (the qt6-tools-dev package, not always installed), so
  # this drives lrelease/lupdate directly. Without lrelease the app is built English-only.
  # `cmake --build <dir> --target murasaki_lupdate` refreshes i18n/*.ts from the sources.
  set(MURASAKI_TS_FILES i18n/murasaki_es.ts)
  get_filename_component(_qt_bin_hint "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" ABSOLUTE)
  find_program(MURASAKI_LRELEASE NAMES lrelease-qt6 lrelease6 lrelease HINTS "${_qt_bin_hint}" /usr/lib/qt6/bin)
  find_program(MURASAKI_LUPDATE NAMES lupdate-qt6 lupdate6 lupdate HINTS "${_qt_bin_hint}" /usr/lib/qt6/bin)
  if(MURASAKI_LRELEASE)
    set(_qm_files)
    foreach(_ts ${MURASAKI_TS_FILES})
      get_filename_component(_name ${_ts} NAME_WE)
      set(_qm ${CMAKE_CURRENT_BINARY_DIR}/${_name}.qm)
      add_custom_command(OUTPUT ${_qm}
        COMMAND ${MURASAKI_LRELEASE} -silent ${CMAKE_CURRENT_SOURCE_DIR}/${_ts} -qm ${_qm}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_ts}
        COMMENT "Compiling ${_ts}"
        VERBATIM)
      list(APPEND _qm_files ${_qm})
    endforeach()
    qt_add_resources(murasaki murasaki_translations PREFIX /i18n BASE ${CMAKE_CURRENT_BINARY_DIR} FILES ${_qm_files})
  else()
    message(STATUS "lrelease not found: the murasaki GUI is built without translations")
  endif()
  if(MURASAKI_LUPDATE)
    add_custom_target(murasaki_lupdate
      COMMAND ${MURASAKI_LUPDATE} ${CMAKE_CURRENT_SOURCE_DIR} -locations none -no-obsolete -source-language en
              -ts ${CMAKE_CURRENT_SOURCE_DIR}/i18n/murasaki_es.ts -target-language es
      COMMENT "Updating translation sources"
      VERBATIM)
  endif()

  install(TARGETS murasaki RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
