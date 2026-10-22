# Credits

## Refract

Refracture XR is a fork of [Refract](https://github.com/MIXIDtheSilly/Refract) by MIXID. The runtimes, the
launcher, the translator patches, the GPU and VR plumbing and the Platform SDK stand-in are Refract's work.
Refracture adds the Unknown sources page and its backend (see [docs/unknown_sources.md](docs/unknown_sources.md)).

## AXRB

Refract started as a fork of [AXRB](https://github.com/Android-XR-Bridge/AXRB) (Android Extended
Reality Bridge) by Feline Reintgen. Some of the original AXRB code, such as parts of the Android
OpenXR runtime, the host bridge and the protocol, is still in this repository, but it has been
renamed and heavily reworked, and Refract has added a great deal of its own: the launcher, the
GPU and GLES layers, the translator performance patches, the Platform SDK stand-in and the
viewer. AXRB is MIT-licensed; its copyright notice is kept in [NOTICE](NOTICE).

## RiftLift

The Meta native-SSO flow in [launcher/core/meta.mjs](launcher/core/meta.mjs)
is adapted from [RiftLift](https://github.com/Villagers654/RiftLift) (GPL-3.0-or-later),
the same license as the rest of Refract; see
[launcher/THIRD_PARTY_NOTICES.md](launcher/THIRD_PARTY_NOTICES.md).

## Other references

- [OpenXR](https://www.khronos.org/openxr/): OpenXR structure and extension declarations
  (Khronos, Apache-2.0 OR MIT).
- [OculusGraphQLApiLib](https://github.com/ComputerElite/OculusGraphQLApiLib): API research
  for the launcher's store integration. No code from it is bundled.
- [Berberis](https://android.googlesource.com/platform/frameworks/libs/binary_translation/)
  (AOSP) and Digitalis (Apache-2.0): the ARM64-to-x86_64 translator. Its prebuilt binaries are
  redistributed in [prebuilts/digitalis/](prebuilts/digitalis/) and, for Waydroid,
  [prebuilts/digitalis-qpr2/](prebuilts/digitalis-qpr2/) (see their NOTICE files), and the
  patches in [tools/translator/](tools/translator/) target it.
- [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (MIT or public domain): the
  Linux viewer's overlay text, in [viewer/stb_truetype.h](viewer/stb_truetype.h).
- [stb_image](https://github.com/nothings/stb) by Sean Barrett (MIT or public domain): decodes the
  loading card's logo on Linux, in [host-bridge/stb_image.h](host-bridge/stb_image.h).
