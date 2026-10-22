# Unknown sources

Games that did not come from the Meta Quest store (your own builds before release, itch.io releases, ports)
can be played without a Meta sign-in. They get their own page, **Unknown sources**, and are kept apart from the
Meta library.

## Using it

- **Add APK** picks one APK. Expansion files (`main.<n>.<package>.obb`, `patch.<n>.<package>.obb`) next to it, or in an
  `obb/<package>` folder beside it, are added with it.
- **Add folder** scans a folder (three levels deep) and adds every APK in it, newest build per package, with the
  expansion files that belong to each.
- **Install / Update** installs the APK, reading it again first so a rebuilt version or launch activity is picked up.
  Saves are kept. The row shows *APK changed since install* when the file on disk is newer than the installed one.
- **Play / details** opens the normal game page: Play in VR, Play on PC, content files, open folder.
- **Pass the game's ownership check** (off by default) makes the Meta Platform stand-in report the game as owned. It is
  for your own builds that run an entitlement check; most itch.io and test builds do not need it.
- The trash button removes the entry from Refract. Files on disk and the installed copy are left alone.

A game whose package is already in your Meta library is not taken over: use that game's page instead.

## Where the code is (so upstream merges stay easy)

Everything lives in new files; the shared files carry small hooks only.

| New file | Role |
| --- | --- |
| `launcher/core/sideload.mjs` | Folder scan, expansion-file matching, library entry, on-disk status |
| `launcher/backend/sideload.mjs` | The `sideload*` backend methods |
| `launcher/src/pages/unknown-sources.jsx`, `launcher/src/unknown-sources.css` | The page |
| `launcher/src/extensions.js` | List of extra pages (the sidebar and router read it) |
| `launcher/src/api/mock-sideload.js` | Sample data for `npm run dev` |
| `launcher/tests/sideload.test.mjs` | Tests |

Hooks in upstream files: `backend/server.mjs` (one import and one `Object.assign(methods, ...)`),
`src-tauri/src/main.rs` (a `sideloadAdd` arm that opens the APK picker), `src/components/chrome.jsx` and
`src/main.jsx` (read `extensions.js`), `src/api/mock.js` (one import and one line). Install and play are Refract's
own, unchanged.

Unknown-source entries are normal library entries with `unknownSource: true` (and `source: 'local'`), so they also
appear in the Library.
