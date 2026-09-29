# Third-party sources

## RiftLift

The Meta native-SSO protocol in `core/meta.mjs` is adapted from
[RiftLift's meta_auth.py](https://github.com/Villagers654/RiftLift/blob/8eddda67d18eace7935d5d385656ed897e4e6c50/src/riftlift/meta_auth.py),
revision `8eddda67d18eace7935d5d385656ed897e4e6c50`, by RiftLift contributors.
RiftLift is licensed under GPL-3.0-or-later. This launcher is distributed under
the same license; see LICENSE. Changes include a JavaScript implementation,
Meta login in a separate private Tauri window, Windows credential encryption, and callback handling
without replacing the user's system-wide Oculus protocol association.

RiftLift's Linux UI, compatibility runtime, and PC game downloader are not bundled.

## API research

Quest GraphQL document IDs and response field names were researched in
[OculusGraphQLApiLib](https://github.com/ComputerElite/OculusGraphQLApiLib),
particularly GraphQLClient.cs and the Application, OculusBinary and AssetFile
response models. No implementation from that project is bundled.

Expansion-file placement follows Meta's Android OBB directory convention:
[Meta expansion files](https://developers.meta.com/horizon/blog/tech-note-expansion-file-support-for-gear-vr-beta/).
Meta checks downloadable asset entitlements on its delivery services:
[Meta mobile DLC](https://developers.meta.com/horizon/blog/introducing-mobile-dlc-support-in-beta/).

Optional patching invokes the user's installed
[ovrport CLI](https://github.com/ovrport/app/tree/master/overportcli) as a separate
program using its documented `patch --input= --output=` interface. No ovrport
binaries are redistributed here. Games and artwork remain their owners' property.

## Desktop shell

Tauri and its plugins are MIT/Apache-2.0 licensed Rust crates, built from source
by Cargo. The window uses the system WebView2 runtime on Windows.

## Interface

The interface in `src/` is original to Refract. React, Radix UI and Vite are MIT
licensed; Lucide icons are ISC licensed. The Jost typeface
(`@fontsource-variable/jost`) is licensed under the SIL Open Font License 1.1.
Dependency licenses are distributed with their npm packages and crates.

Quest delivery's application-token exchange was verified against the public
[QuestAppVersionSwitcher LoginClient API flow](https://github.com/ComputerElite/QuestAppVersionSwitcher/blob/master/QuestAppVersionSwitcher/OculusGraphQLApiLib/GraphQL/LoginClient.cs).
Only endpoint and application-ID protocol facts are used; its implementation is
not bundled. Download tokens remain in the backend process and are not persisted.
