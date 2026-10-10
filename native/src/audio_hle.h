// Audio for the guest: libaaudio.so and the shim's android.media.AudioTrack / AudioRecord, both on
// WASAPI streams of the default Windows playback and recording devices.
#pragma once

#include <jni.h>

namespace rn {

void RegisterAudioHle();
// Registers refract.media.HostAudio natives (after the JVM starts).
bool RegisterAudioNatives(JNIEnv* env);

}  // namespace rn
