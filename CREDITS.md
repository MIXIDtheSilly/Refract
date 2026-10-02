# Credits

## AXRB

Refract is based on [AXRB](https://github.com/Android-XR-Bridge/AXRB) (Android Extended
Reality Bridge) by Feline Reintgen. Much of the code in this repository uses
AXRB code: the Android OpenXR runtime, the runtime APK, the host bridge, the protocol,
the tests and the launcher. That code was renamed and then extended in this repository.
AXRB is MIT-licensed; its copyright notice is kept in [LICENSE](LICENSE).

## RiftLift

The Meta native-SSO flow in [launcher/core/meta.mjs](launcher/core/meta.mjs)
is adapted from [RiftLift](https://github.com/Villagers654/RiftLift) (GPL-3.0-or-later).
Because of this, the launcher is distributed under GPL-3.0-or-later; see
[launcher/LICENSE](launcher/LICENSE) and
[launcher/THIRD_PARTY_NOTICES.md](launcher/THIRD_PARTY_NOTICES.md).

## Other references

- [OpenXR](https://www.khronos.org/openxr/): OpenXR structure and extension declarations
  (Khronos, Apache-2.0 OR MIT).
- [OculusGraphQLApiLib](https://github.com/ComputerElite/OculusGraphQLApiLib): API research
  for the launcher's store integration. No code from it is bundled.
- [Berberis](https://android.googlesource.com/platform/frameworks/libs/binary_translation/)
  (AOSP) and Digitalis (Apache-2.0): the ARM64-to-x86_64 translator. Its prebuilt binaries are
  redistributed in [prebuilts/digitalis/](prebuilts/digitalis/) (see its NOTICE), and the
  patches in [tools/translator/](tools/translator/) target it.
