#pragma once

#include <QString>

namespace murasaki {

inline constexpr QLatin1String kWallpaperEngineAppId("431960");
inline constexpr QLatin1String kAnimaEngineAppId("3474900");

inline constexpr QLatin1String kDBusService("io.github.uriel2121ger_art.Murasaki");
inline constexpr QLatin1String kDBusPath("/io/github/uriel2121ger_art/Murasaki");
// Keep in sync with Q_CLASSINFO("D-Bus Interface") in murasakiservice.h (it needs a literal).
inline constexpr QLatin1String kDBusInterface("io.github.uriel2121ger_art.Murasaki1");
// The revision the daemon advertises. Revision 5 added pets (placements, transcode cache, arrange mode); revision 6
// SetOutputScales and the per-bucket transcodes; revision 7 playlists, spanning and the pause policy settings;
// revision 8 SetItemPermission and the permissions of an item; revision 9 the SteamCMD calls; revision 10 says why a
// wallpaper is not shown (the surface's Outputs() "failure" and RetryOutput; the daemon has no new call);
// revision 11 the pets' timing (placement "speed", "fps" up to 120) and the settings petMaxFps and petSaveEnergy;
// revision 12 the pets' behaviour fields (clickThrough, locked, keepAbove, hoverHide, focusFade, smartDodge, rotation, flips), PetsHidden and RescuePets;
// revision 13 the interaction with the wallpaper (WallpaperInteract, SetWallpaperInteract);
// revision 14 the pet imports (ImportPet, PreviewImport, ImportState, CancelImport, RemoveImportedPet, StartScene, ImportChanged, and "generated" in an item).
// revision 15 the pets' sound (placement "audio", the settings petsVolume, petsMuted and petsMuteWhenFullscreen, the audio asset calls, PetAudioMuted on the surface);
// revision 16 the surface's per-output session facts (SessionFacts and OutputFullscreen: which output has a fullscreen window, which applications are open);
// revision 17 the fullscreen animation mode of a pet (placement "stage", the settings stageHideOverFullscreen and stageMaxPixels, Windows() stage fields, RescuePets turns stages off);
// revision 18 the automation (profiles, triggers, timelines, the pets' runtime layer; phase 7 of the Anima parity);
// revision 19 the pets' states and Mouse Tracking (placement "states" and "tracking", Windows() state fields, RescuePets turns tracking off; phase 3);
// revision 20 the puppet pets (AddLocalPuppetItem, RemoveLocalPuppetItem, the item's "format": "puppet" and "puppet" object, Windows() "puppet" fields; phase 8);
// revision 21 the screen triggers (StartScreenWatch, StopScreenWatch, CaptureScreenReference, CommitScreenReference, ScreenWatchState, ScreenWatchChanged, ScreenCaptured, the settings screen*; phase 9);
// revision 22 the demonstrations (the "track" action of a timeline and "tracking" in the runtime layer, SetProfileOrigin; phase 10; 21 is phase 9's);
// revision 23 the idle trigger's "state" (away|back) and the idle fact of the surface (ext-idle-notify);
// revision 24 the setting petsHideOnGame (the pets on a game's screen hide while it plays).
inline constexpr quint32 kDBusApiVersion = 24;
// How long without input the surface waits before it says the session is idle (ext-idle-notify-v1). The daemon counts the idle from the last input (the notification comes
// this long after it), so the number only has to be at most the shortest idle trigger, 1 minute; shorter would make the compositor wake the surface for naps nobody asked for.
inline constexpr int kIdleNotifyTimeoutMs = 60'000;
// The most the surface's "idleAfterMs" is believed (it is the timeout above; this is the bound for a surface that says nonsense).
inline constexpr qint64 kMaxIdleAfterMs = 3'600'000;
// The oldest daemon revision the clients (GUI, surface, Plasma plugin) work with; only the pet features need 5.
inline constexpr quint32 kMinDBusApiVersion = 4;
// The first revision with the pet calls (ListPets, AddPet, ..., AddLocalPetItem).
inline constexpr int kPetsDBusApiVersion = 5;
// The first revision with SetOutputScales (the overlay works against 5 without it).
inline constexpr quint32 kOutputScalesDBusApiVersion = 6;
// The pets' timing (placement speed, fps above 60) and the settings petMaxFps and petSaveEnergy.
inline constexpr quint32 kTimingDBusApiVersion = 11;
// The pets' behaviour fields, PetsHidden and RescuePets.
inline constexpr quint32 kPetBehavioursDBusApiVersion = 12;
// WallpaperInteract and SetWallpaperInteract.
inline constexpr quint32 kWallpaperInteractDBusApiVersion = 13;
// The first revision with the pet imports.
inline constexpr quint32 kImportDBusApiVersion = 14;
// The pets' sound.
inline constexpr quint32 kPetAudioDBusApiVersion = 15;
// The fullscreen animation mode (stage).
inline constexpr quint32 kPetStageDBusApiVersion = 17;
// Profiles, triggers, timelines and the pets' runtime layer.
inline constexpr quint32 kAutomationDBusApiVersion = 18;
// The pets' states and Mouse Tracking.
inline constexpr quint32 kPetStatesDBusApiVersion = 19;
// The puppet pets (AddLocalPuppetItem, RemoveLocalPuppetItem, the item's "format" and "puppet", Windows() "puppet").
inline constexpr quint32 kPuppetsDBusApiVersion = 20;
// The screen triggers (revision 21; 22 came first, so kDBusApiVersion is the newer of the two).
inline constexpr quint32 kScreenDBusApiVersion = 21;
// The demonstrations: the "track" action, the runtime layer's "tracking" and SetProfileOrigin.
inline constexpr quint32 kDemosDBusApiVersion = 22;
// The idle trigger's "state" (`away`, the default, or `back`).
inline constexpr quint32 kIdleBackDBusApiVersion = 23;
// The setting petsHideOnGame.
inline constexpr quint32 kPetsGameDBusApiVersion = 24;
// The first revision with playlists (ListPlaylists, ..., NextInPlaylist) and "playlist:<id>" assignments.
inline constexpr quint32 kPlaylistsDBusApiVersion = 7;
// The first revision with SetItemPermission and "permissions" in the item JSON.
inline constexpr quint32 kPermissionsDBusApiVersion = 8;
// The first revision with SteamCmdStatus, SteamCmdLogin, ..., SteamCmdCancel.
inline constexpr quint32 kSteamCmdDBusApiVersion = 9;
// The first revision whose surface reports failures (Outputs() "failure") and takes RetryOutput.
inline constexpr quint32 kEngineHealthDBusApiVersion = 10;
// A wallpaper that has drawn nothing after this long is reported as failed (a late frame still clears it).
inline constexpr int kEngineNoFrameTimeoutMs = 15000;

inline constexpr QLatin1String kSurfaceDBusService("io.github.uriel2121ger_art.Murasaki.Surface");
inline constexpr QLatin1String kSurfaceDBusPath("/io/github/uriel2121ger_art/Murasaki/Surface");
// Keep in sync with Q_CLASSINFO("D-Bus Interface") in src/surface/surfaceservice.h.
inline constexpr QLatin1String kSurfaceDBusInterface("io.github.uriel2121ger_art.Murasaki.Surface1");

// murasaki-overlay (the desktop pets). It owns this name, which is also how D-Bus activation starts it.
inline constexpr QLatin1String kOverlayDBusService("io.github.uriel2121ger_art.Murasaki.Overlay");
inline constexpr QLatin1String kOverlayDBusPath("/io/github/uriel2121ger_art/Murasaki/Overlay");
// Keep in sync with Q_CLASSINFO("D-Bus Interface") in src/overlay/overlayservice.h.
inline constexpr QLatin1String kOverlayDBusInterface("io.github.uriel2121ger_art.Murasaki.Overlay1");

// The default global shortcut that toggles the arrange mode of the pets, as text for the user (the overlay binds
// the key code; keep them in sync).
inline constexpr QLatin1String kArrangeShortcutText("Meta+Shift+P");

// Global settings: the defaults (also what the surface uses until it has read the daemon's settings, or when
// the daemon predates them) and the accepted range of the web frame rate limit.
inline constexpr int kDefaultWebFpsLimit = 30;
inline constexpr bool kDefaultPauseWhenCovered = true;
inline constexpr int kMinWebFpsLimit = 10;
inline constexpr int kMaxWebFpsLimit = 144;

// Pause rules (revision 7): pause while a game runs (GameMode), what to do on battery, and the applications whose
// focus or fullscreen pauses the wallpaper. See src/surface/pausepolicy.h for what they mean.
inline constexpr bool kDefaultPauseOnGame = true;
inline constexpr QLatin1String kBatteryModeOff("off");
inline constexpr QLatin1String kBatteryModePause("pause");
inline constexpr QLatin1String kBatteryModeLowFps("lowfps");
inline constexpr QLatin1String kDefaultBatteryMode = kBatteryModeOff;
// Run the scene engines inside the Landlock sandbox (murasaki-sandbox); off only for debugging.
inline constexpr bool kDefaultSandboxEngines = true;
// Refuse to start a scene engine that would run without the sandbox (no helper, no Landlock, TCP not refusable). Off by
// default: a wallpaper that plays unconfined beats a black screen for most people.
inline constexpr bool kDefaultRequireSandbox = false;
inline constexpr int kDefaultPetMaxFps = 0;           // the desktop pets are not limited
inline constexpr int kDefaultPetsVolume = 100;               // the master volume of the pets' sounds (their own are off until chosen)
inline constexpr bool kDefaultPetsMuted = false;
inline constexpr bool kDefaultPetsMuteWhenFullscreen = true;
inline constexpr bool kDefaultPetsHideOnGame = false; // the pets stay on a game's screen unless asked
inline constexpr bool kDefaultPetSaveEnergy = true;   // but are held to a low rate on battery or in the power-saver profile
inline constexpr bool kDefaultShortcutsLaunch = true; // a click on a dock shortcut of a scene starts the application the user chose for it
inline constexpr bool kDefaultMediaShare = true; // the scenes that use the media (cover, events) are told what plays
inline constexpr bool kDefaultMediaRemoteArt = true; // https covers of the media player are downloaded by the host (never by the engine)

// The resolution the Vulkan scene engine draws at, as target pixels per output pixel (1: as many as the output has), or 0:
// automatic, which lowers the resolution (never below half) while the GPU needs more than a quarter of each frame.
inline constexpr double kMinSceneRenderScale = 0.25;
inline constexpr double kMaxSceneRenderScale = 1.0;
inline constexpr double kDefaultSceneRenderScale = 0.0;
inline constexpr int kMaxPauseApps = 64;
inline constexpr int kMaxPauseAppLength = 100;

inline bool isValidBatteryMode(const QString &value)
{
    return value == kBatteryModeOff || value == kBatteryModePause || value == kBatteryModeLowFps;
}

// Who draws the wallpapers: the surface's own layer-shell windows, the Plasma plugin (hosts), or whichever the
// session supports ("auto").
inline constexpr QLatin1String kPresenterAuto("auto");
inline constexpr QLatin1String kPresenterLayerShell("layer-shell");
inline constexpr QLatin1String kPresenterPlasma("plasma");
inline constexpr QLatin1String kDefaultPresenter = kPresenterAuto;

inline bool isValidPresenter(const QString &value)
{
    return value == kPresenterAuto || value == kPresenterLayerShell || value == kPresenterPlasma;
}

// Which engine draws scene wallpapers: the own Vulkan engine murasaki-scene-vk ("vulkan", needs it installed) or "auto",
// the default, which is the same engine. A missing setting means "auto".
inline constexpr QLatin1String kSceneEngineAuto("auto");
inline constexpr QLatin1String kSceneEngineVulkan("vulkan");
inline constexpr QLatin1String kDefaultSceneEngine = kSceneEngineAuto;

inline bool isValidSceneEngine(const QString &value)
{
    return value == kSceneEngineAuto || value == kSceneEngineVulkan;
}

} // namespace murasaki
