#pragma once

namespace refract::runtime {

// Per-game compatibility fixes applied to the app's own code in memory (the APK is never changed).
// Each patch checks the exact instruction bytes first and does nothing on any other build.
// debug.refract.game_patches=0 disables all of them. Safe to call repeatedly.
void apply_game_patches();

} // namespace refract::runtime
