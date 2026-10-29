#pragma once

#include <QString>

namespace murasaki {

// The own Vulkan scene engine (murasaki-scene-vk), first match wins: $MURASAKI_SCENE_VK (when set it is the only
// candidate: it must name an executable file), `murasaki-scene-vk` on PATH, ~/.local/bin/murasaki-scene-vk,
// ~/.local/lib/murasaki/scene/murasaki-scene-vk. Empty when none exists. Looked up on every call.
QString findVulkanSceneEngine();

// murasaki-sandbox, the Landlock helper that confines a scene engine before it runs (src/sandbox), first match wins:
// $MURASAKI_SANDBOX (when set it is the only candidate), next to the running executable (or in a sibling sandbox/ build
// directory), `murasaki-sandbox` on PATH, ~/.local/bin/murasaki-sandbox. Empty when none exists. Looked up on every call.
QString findSandboxHelper();

// murasaki-petimport, the helper that imports pets (src/import): $MURASAKI_PETIMPORT, next to the program or in the build tree's src/import, on PATH, then ~/.local/bin.
// Empty when there is none (the imports then fail with "helper failed").
QString findPetImportHelper();
// murasaki-petaudio, the player and the sound importer: MURASAKI_PETAUDIO, next to the program (or in a build tree's src/audio), in PATH, in ~/.local/bin.
QString findPetAudioHelper();
// murasaki-screenmatch, the screen matching helper (src/screenmatch): MURASAKI_SCREENMATCH, next to the program (or in a build tree's src/screenmatch), in PATH, in ~/.local/bin.
QString findScreenMatchHelper();
// murasaki-puppet, the puppet engine and its `--check` (src/puppet): MURASAKI_PUPPET, next to the program (or in a build tree's src/puppet), in PATH, in ~/.local/bin.
QString findPuppetHelper();

} // namespace murasaki
