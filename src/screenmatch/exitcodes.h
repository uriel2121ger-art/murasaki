#pragma once

namespace murasaki::screenmatch {

// The exit status of murasaki-screenmatch, so that whoever starts it (src/overlay/screenmatchhost.h) can tell a crash from an ending it expected.
inline constexpr int kExitOk = 0;           // the daemon said quit or closed the channel
inline constexpr int kExitFailure = 1;      // a runtime failure: counts as a crash
inline constexpr int kExitUsage = 2;        // the command line was wrong: a bug of the caller, counts as a crash
inline constexpr int kExitNoPipeWire = 3;   // this build has no PipeWire, or the descriptor is not a PipeWire remote: not a crash, no use restarting
inline constexpr int kExitStreamLost = 4;   // the desktop ended the share: not a crash, matching stops until the user starts it again
inline constexpr int kExitChannelStalled = 5; // the daemon did not read the helper's answers for seconds (a stalled GUI): not a crash and not counted, matching stops
// murasaki-sandbox itself: 125 it could not set the sandbox up (or was called wrongly), 127 the program was not found. Neither is the helper's.
inline constexpr int kExitSandboxUnavailable = 125;
inline constexpr int kExitProgramNotFound = 127;

enum class ExitClass { Clean, NoPipeWire, StreamLost, SandboxUnavailable, ChannelStalled, Crash };

inline constexpr ExitClass classifyExit(bool killedBySignal, int code)
{
    if (killedBySignal)
        return ExitClass::Crash;
    switch (code) {
    case kExitOk: return ExitClass::Clean;
    case kExitNoPipeWire: return ExitClass::NoPipeWire;
    case kExitStreamLost: return ExitClass::StreamLost;
    case kExitChannelStalled: return ExitClass::ChannelStalled;
    case kExitSandboxUnavailable:
    case kExitProgramNotFound: return ExitClass::SandboxUnavailable;
    default: return ExitClass::Crash;
    }
}

} // namespace murasaki::screenmatch
