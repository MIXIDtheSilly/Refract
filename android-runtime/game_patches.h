#pragma once

namespace refract::runtime {

// Per-game compatibility fixes applied to the app's own code in memory (the APK is never changed).
// Each patch checks the exact instruction bytes first and does nothing on any other build.
// debug.refract.game_patches=0 disables all of them. Safe to call repeatedly.
void apply_game_patches();

// debug.refract.texture_mip_limit=N (N > 0) raises Unity's QualitySettings.globalTextureMipmapLimit
// to at least N through the game's own IL2CPP icall: textures upload without their top N mip levels,
// so they need about 4^-N of the host GPU memory. The emulator expands ASTC to RGBA8 (16x for 8x8
// blocks), which overflowed a 10 GB card in Batman: Arkham Shadow. Call from any thread; it only acts
// on UnityMain, at most once a second, and re-applies if the game lowers the limit again.
void apply_unity_quality();

} // namespace refract::runtime
