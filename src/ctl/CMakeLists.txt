add_library(murasakictl_logic STATIC ctlutil.cpp ctlutil.h screenctl.cpp screenctl.h)
set_target_properties(murasakictl_logic PROPERTIES AUTOMOC ON)
target_include_directories(murasakictl_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(murasakictl_logic PUBLIC murasaki_core Qt6::DBus)

add_executable(murasakictl main.cpp)
target_link_libraries(murasakictl PRIVATE murasakictl_logic murasaki_plasma Qt6::DBus)
install(TARGETS murasakictl)
