add_library(murasakid_service STATIC
  automationmanager.cpp automationmanager.h
  automationsources.cpp automationsources.h
  buslease.cpp buslease.h
  murasakiservice.cpp murasakiservice.h
  murasakiservicelibrary.cpp murasakiservicepetimport.cpp murasakiservicepets.cpp
  murasakiservicetranscode.cpp murasakiserviceworkshop.cpp murasakiserviceinternal.h
  murasakiserviceautomation.cpp
  murasakiservicescreen.cpp
  overlayunit.cpp overlayunit.h
  petaudiomanager.cpp petaudiomanager.h
  puppetchecker.cpp puppetchecker.h
  screencastportal.cpp screencastportal.h
  screenwatch.cpp screenwatch.h
  petimportmanager.cpp petimportmanager.h
  playlistmanager.cpp playlistmanager.h
  ptyprocess.cpp ptyprocess.h
  steamcmdmanager.cpp steamcmdmanager.h
  steamcmdparser.cpp steamcmdparser.h
  transcodemanager.cpp transcodemanager.h
  workshopmetadataclient.cpp workshopmetadataclient.h)
target_include_directories(murasakid_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui) # (image I/O only, no window: the references and the explicit capture are PNGs)
target_link_libraries(murasakid_service PUBLIC murasaki_import_logic murasaki_core murasaki_shortcut murasaki_sessionwatch murasaki_screenmatch_host murasaki_screenmatch_logic Qt6::DBus Qt6::Network Qt6::Gui)

add_executable(murasakid main.cpp)
target_link_libraries(murasakid PRIVATE murasakid_service)
install(TARGETS murasakid)
