#pragma once

namespace murasaki::puppet {

// The exit status of `murasaki-puppet --engine`: why the process ended, so that the restart policy of whoever started it (src/overlay/puppetenginehost.h) can tell a crash from a refusal.
inline constexpr int kExitOk = 0;           // the host closed the socket or sent Quit
inline constexpr int kExitFailure = 1;      // a runtime failure (the bridge, an unexpected exception): counts as a crash
inline constexpr int kExitUsage = 2;        // the command line was wrong: a bug of the caller, counts as a crash
inline constexpr int kExitModelRefused = 3; // the model cannot be shown (not an error of the engine: not a crash)
inline constexpr int kExitNoGpu = 4;        // no usable Vulkan device: not a crash, and no use restarting
inline constexpr int kExitDeviceLost = 5;   // the GPU went away under the engine: counts as a crash
// murasaki-sandbox itself: 125 it could not set the sandbox up (or was called wrongly), 127 the program was not found. Neither is the engine's.
inline constexpr int kExitSandboxUnavailable = 125;
inline constexpr int kExitProgramNotFound = 127;

// What an end of the process means for the restart policy (shared by PuppetEngineHost and the overlay's PuppetItem, so that they cannot disagree).
enum class ExitClass {
    Clean,              // 0: the socket was closed or Quit was sent
    ModelRefused,       // 3: not retried, not a crash
    NoGpu,              // 4: not retried, not a crash
    SandboxUnavailable, // 125, 127: not retried, not a crash
    Crash,              // a signal, or any other status
};

inline constexpr ExitClass classifyExit(bool killedBySignal, int code)
{
    if (killedBySignal)
        return ExitClass::Crash;
    switch (code) {
    case kExitOk: return ExitClass::Clean;
    case kExitModelRefused: return ExitClass::ModelRefused;
    case kExitNoGpu: return ExitClass::NoGpu;
    case kExitSandboxUnavailable:
    case kExitProgramNotFound: return ExitClass::SandboxUnavailable;
    default: return ExitClass::Crash;
    }
}

} // namespace murasaki::puppet
