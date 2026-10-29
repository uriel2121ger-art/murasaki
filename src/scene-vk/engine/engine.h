#pragma once

// The engine process: opens the bridge, builds the renderer for the item and runs the frame loop until the
// consumer quits or goes away.

#include "engine/options.h"

namespace murasaki::scene {

// Exit status after a lost GPU (VK_ERROR_DEVICE_LOST); 1 is any other failure, 0 a normal end.
inline constexpr int kExitDeviceLost = 3;

// Runs host mode (--murasaki-bridge-fd). Returns the process exit status: 0 after Quit or when the consumer closed
// the socket, 1 on a runtime failure (an Error message was sent when the socket still worked).
int runEngine(const Options &options);

} // namespace murasaki::scene
