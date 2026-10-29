# File logs with size rotation (Qt Core only). Its own library because the Plasma wallpaper's Murasaki.Bridge module links it
# too, through murasaki_bridge_client, and can link nothing else of ours.
add_library(murasaki_filelog STATIC filelog.cpp filelog.h)
target_include_directories(murasaki_filelog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_filelog PUBLIC Qt6::Core)
set_target_properties(murasaki_filelog PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(murasaki_core STATIC
  animaengineimporter.cpp animaengineimporter.h
  animationinfo.cpp animationinfo.h
  assignment.cpp assignment.h
  automation.cpp automation.h
  automationsettings.cpp automationsettings.h
  automationstore.h
  autostart.cpp autostart.h
  demos.cpp demos.h
  buildinfo.cpp buildinfo.h
  canvas.cpp canvas.h
  constants.h
  jsonfile.cpp jsonfile.h
  itemrisks.cpp itemrisks.h
  itempermissions.cpp itempermissions.h
  library.cpp library.h
  localpets.cpp localpets.h
  localpuppets.cpp localpuppets.h
  packautomation.cpp packautomation.h
  petaudio.cpp petaudio.h
  petcheck.cpp petcheck.h
  petlimits.h
  petruntime.cpp petruntime.h
  pettiming.cpp pettiming.h
  petstage.cpp petstage.h
  petstates.cpp petstates.h
  stagegeometry.cpp stagegeometry.h
  petstore.cpp petstore.h
  puppetsettings.cpp puppetsettings.h
  playlist.cpp playlist.h
  profile.cpp profile.h
  profileswitch.cpp profileswitch.h
  timeline.cpp timeline.h
  timelinerunner.cpp timelinerunner.h
  trigger.cpp trigger.h
  screenmatch.cpp screenmatch.h
  screenrefstore.cpp screenrefstore.h
  securebytes.cpp securebytes.h
  triggerengine.cpp triggerengine.h
  pathutil.cpp pathutil.h
  sandboxreport.cpp sandboxreport.h
  sceneenginelookup.cpp sceneenginelookup.h
  importablefiles.cpp importablefiles.h
  importsettings.cpp importsettings.h
  settings.cpp settings.h
  transcodebuckets.cpp transcodebuckets.h
  steamlocator.cpp steamlocator.h
  vdf.cpp vdf.h
  wallpaperengineimporter.cpp wallpaperengineimporter.h
  wallpaperitem.cpp wallpaperitem.h
  weproperty.cpp weproperty.h
  workshopstate.cpp workshopstate.h
  workshopwatcher.cpp workshopwatcher.h)
# The pixel loops of the screen matcher are the hot path of a tick; -O3 lets the compiler vectorise them.
set_source_files_properties(screenmatch.cpp PROPERTIES COMPILE_OPTIONS -O3)
target_compile_definitions(murasaki_core PRIVATE MURASAKI_VERSION="${PROJECT_VERSION}")
target_include_directories(murasaki_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Window class spellings (header only), shared with the surface's pause rules and the shortcut catalogue.
add_library(murasaki_appclass INTERFACE)
target_include_directories(murasaki_appclass INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_appclass INTERFACE Qt6::Core)

target_link_libraries(murasaki_core PUBLIC Qt6::Core murasaki_appclass murasaki_filelog PRIVATE murasaki_puppet_logic)

# Switching the Plasma wallpaper (murasakictl plasma, the GUI toggle). The logic is backend-agnostic and tested with a
# fake; only the D-Bus backend needs Qt D-Bus.
add_library(murasaki_plasma STATIC plasmacontrol.cpp plasmacontrol.h plasmadbus.cpp plasmadbus.h)
target_include_directories(murasaki_plasma PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_plasma PUBLIC murasaki_core Qt6::DBus)

# Whether the session is locked or the screen is off (the pets pause on it, the playlists do not rotate during it).
add_library(murasaki_sessionwatch STATIC sessionwatch.cpp sessionwatch.h)
target_include_directories(murasaki_sessionwatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasaki_sessionwatch PUBLIC Qt6::Core Qt6::DBus)
