# bridge-view: a headless bridge consumer for the host-mode integration test (tools/test-host-nested.sh). It
# launches a producer, imports the frames it sends (dma-buf or shared memory), writes PNGs and checks that the
# picture is not black, that it animates, that Pause stops it and that Resize changes it.
add_executable(bridge-view main.cpp)
target_include_directories(bridge-view PRIVATE ${PROJECT_SOURCE_DIR}/tests) # eglcontext.h: the surfaceless context
target_link_libraries(bridge-view PRIVATE Qt6::Gui murasaki_bridge_client murasaki_dmabuf)
