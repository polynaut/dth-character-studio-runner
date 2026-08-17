# DTH Character Studio Runner

A Daz Studio C++ plugin that runs [DTH Character Studio](https://github.com/polynaut/dth-character-studio)
export batches unattended. The studio writes a small JSON job file; this
plugin watches for it (real file watching since v1.3.0, with a fallback
poll), renames it as the "started" signal, executes every row and keeps the
file's progress current — no clicking through scenes and scripts by hand.

The normative contract lives in the studio repo:
`dth-character-studio/docs/exporter-plugin-job-file.md`. Summary (v2+v3):

- **Job file:** `dth_exporter_jobs.json` directly inside
  `<content dir>/Scripts/DTH-Character-Studio/`. The plugin probes **every
  mapped Daz content directory** and processes the first file it finds.
- **Format:** UTF-8 JSON — `{version: 1, type: "bulk-export", progress,
  progressLogPath?, jobs: [{scenePath, scriptPath, steps?, status, error?}]}`.
  An empty `scenePath` means "run in a new empty scene". Unknown fields are
  ignored (forward compatibility). A file with another version/type is
  foreign: left in place, warned about once, skipped until it changes.
- **Verbose progress log (v1.2.0):** when the job carries `progressLogPath`,
  the plugin truncates that file at batch start and appends
  `[<percent>] <message>` lines as it works. The percent is **per scene**:
  each row's export is `steps` equal steps (the row's `steps`, e.g. 5 = open
  scene / generate ROM / export character / export hair / deliver CSV). The
  plugin reports the steps it owns — `[0] <scene>: opening scene`,
  `[100/steps] <scene>: scene opened`, terminal `[100] <scene>: done|failed —
  <reason>` — and the studio-generated export script appends the interior
  steps to the same file on the same scale while `DzScript::execute()` runs.
  Without `progressLogPath` nothing is written; the whole-batch `progress`
  field in the job file keeps working regardless (older studios rely on it).
- **Pickup (v1.3.0):** a `QFileSystemWatcher` on every content dir's
  `Scripts/DTH-Character-Studio/` makes the pickup near-instant (debounced a
  beat — the studio writes temp-file-then-rename, so one handoff is a burst
  of events). The classic poll stays underneath as the safety net: slow
  (15 s) while the watches cover every existing scripts dir, the old 5 s
  whenever they can't — a scripts dir that doesn't exist yet can't be watched
  (the fallback tick re-syncs the watch list, so it arms when the dir
  appears), and change notification on network shares is best-effort.
- **Lifecycle:** notice the file (watch event or poll tick; the first check
  starts a few seconds after launch) →
  **rename** to `running_dth_exporter_jobs.json` (the "started" signal — the
  studio can only abort an un-renamed file; a stale `running_` leftover is
  cleared first) → per row: open the scene with a no-save replace (or
  explicitly start a new empty scene) → run the row's `.dsa` (plain
  `DzScript::execute()`, no arguments — script arguments never reach
  `getArguments()`, measured; the studio's job rows point at a dedicated bulk
  script instead) → update the row's status + the whole-batch `progress` in
  the renamed file → next row. At the end the plugin writes `progress: 100`
  and LEAVES the file — the studio reads the outcome and deletes it. Per-row
  failures are logged, marked `failed` (+ `error`) and skipped. Watching and
  polling are suspended while a batch runs; the batch's end queues one
  immediate re-check, so a handoff written mid-batch starts right away.
- **`type: "open-scene"` (contract v3, v1.1.0+):** the same envelope carrying
  ONE script-less row — the studio's "Open in Daz" for an already-running
  instance (Daz drops forwarded command-line opens once a scene is loaded).
  The plugin loads the scene (no-save replace), **raises the Daz main window**
  (impossible from outside the process), marks the row done and writes
  `progress: 100` — and, deliberately, does NOT end on a new empty scene: the
  loaded scene is the point. Any other shape (several rows, empty scenePath)
  is foreign. Since v1.1.4 the freshly loaded scene is also marked **unmodified**
  (`DzScene::assetSaved()`): Daz flags a just-loaded scene as needing a save, so
  closing it asked to save changes nobody had made.
- **Legacy:** the old `dth_exporter_jobs.csv` (contract v1) keeps its
  parse → delete-as-ack → run lifecycle for older studio versions.
- **Never saves a scene.** The ROM keyframes a script creates are throwaway
  working state; the plugin ends every export batch on a fresh empty scene
  (open-scene batches excepted, see above).
- **Never silently discards the USER'S scene either (v1.1.2+):** when the open
  scene has unsaved changes as a batch starts (row 0 / an open-scene job), the
  plugin raises the Daz window and shows Daz's own Save Changes choice
  (Yes / No / Cancel; Yes runs the regular File > Save, incl. Save As for an
  unsaved scene). Cancel — or cancelling the save — cancels the batch and
  **deletes the job file** (v1.1.3): a deliberate cancel is not an outcome to
  report, nothing lingers, nothing re-runs. Later rows replace only the
  previous row's throwaway keyframes and never prompt.

All activity is logged to the Daz Studio log (Help → Troubleshooting →
View Log File) with the prefix `[DTH Character Studio Runner]`. A manual trigger is
registered as the action **"DTH Character Studio Runner: Check for Jobs Now"**
(add it to a menu/toolbar via Window → Workspace → Customize).

## Install from a release

Grab the zip for your Daz Studio from the
[Releases page](https://github.com/polynaut/dth-character-studio-runner/releases):

- **Daz Studio 6.25+**: `dth-character-studio-runner-<version>-ds6.zip` → copy
  `dsp_dthcharacterstudiorunner.dll` into `<DAZStudio6>\plugins\` (admin rights; keep the
  `dsp_` name — DS6 only loads plugins named `dsp_*.dll`)
- **Daz Studio 4.x**: `dth-character-studio-runner-<version>-ds4.zip` → copy
  `dthcharacterstudiorunner.dll` into `<DAZStudio4>\plugins\`

Restart Daz Studio and verify under Help → About Installed Plugins. The
`.pdb` in the zip is optional (crash symbols only).

## Requirements

- Windows, 64-bit
- **Daz Studio 6.25+** (primary target) and/or Daz Studio 4.x
- **Visual Studio 2022** (Build Tools are enough — MSVC v143 + Windows SDK)
- The matching **Daz Studio SDK** (free store products, installed via
  Daz Install Manager):
  - *Daz Studio 6.x SDK* (beta) for the DS6 build — prefer an SDK **no newer
    than the oldest Daz Studio 6** the DLL should load in (a newer SDK's
    import lib may reference dzcore exports an older Studio lacks; releases
    build against the oldest supported 6.25 SDK, see the `release.ps1`
    default). Also note: the SDK's **Windows plugin macro exports C++-mangled
    entry points** that Daz cannot resolve (*"load failed - could not locate
    the getSDKVersion() function"*) — `pluginmain.cpp` pre-declares
    `getSDKVersion`/`getPluginDefinition` as `extern "C"` to fix that; read
    the comment there before touching it.
  - *DAZ Studio 4.5+ SDK* for the DS4 build (bundles its own Qt 4.8)
- For the DS6 build only: a **Qt 6.10.x msvc2022 x64 devkit** matching the
  Qt bundled inside Daz Studio 6 (6.10.3 for DS 6.25). No Qt account needed:

  ```powershell
  pip install aqtinstall
  python -m aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 -O C:\Qt
  ```

The DS6 SDK ships no Qt import libraries — at runtime the plugin resolves Qt
against Daz Studio's own bundled Qt DLLs already loaded in-process; the devkit
is needed at build time only.

## Build

```powershell
# parser tests only (no SDK needed) — same thing CI runs
.\build.ps1 -TestsOnly

# Daz Studio 6 plugin  →  build-sdk6\Release\dsp_dthcharacterstudiorunner.dll
.\build.ps1 -SdkDir "<path to Daz Studio 6.x SDK>" -QtDir C:\Qt\6.10.3\msvc2022_64

# Daz Studio 4 plugin  →  build-sdk4\Release\dthcharacterstudiorunner.dll
.\build.ps1 -SdkDir "<path to DAZStudio4.5+ SDK>" -SdkVersion 4
```

Install by copying the DLL into the Daz plugins folder (admin shell), or add
`-Install`:

```powershell
.\build.ps1 -SdkDir "<sdk6>" -QtDir C:\Qt\6.10.3\msvc2022_64 -Install
```

Note the DLL naming: Daz Studio 6 **only loads plugins named `dsp_*.dll`**;
Daz Studio 4 plugins carry no prefix. The build sets this automatically.

## Verify it loaded

1. Start Daz Studio → Help → About Installed Plugins → "DTH Character Studio Runner".
2. The log shows `[DTH Character Studio Runner] watching content directories for
   /Scripts/DTH-Character-Studio/dth_exporter_jobs.csv (every 5 s)`.
3. Drop a hand-written job file into
   `<My DAZ 3D Library>\Scripts\DTH-Character-Studio\` pointing at a trivial
   `.dsa`; it should be consumed within ~5 seconds and the script executed.

## Development

- `src/CsvJobFile.{h,cpp}` — pure C++17 parser (no Qt/SDK), unit-tested in
  `tests/test_csvjobfile.cpp`; runs on every push via GitHub Actions.
- `src/JsonJobFile.{h,cpp}` — pure C++17 reader/writer for the v2 JSON job
  file (hand-rolled: the DS4 build is Qt 4.8, no QJsonDocument), unit-tested
  in `tests/test_jsonjobfile.cpp`; same CI.
- `src/JobPoller.{h,cpp}` — main-thread pickup (QFileSystemWatcher + fallback
  QTimer) + queued-slot batch state machine. All Daz API and `DzScript` calls
  stay on the main thread (a hard SDK rule), which main-thread Qt signal
  delivery gives us for free.
- `src/JobRunnerAction.{h,cpp}` — registered `DzAction`; its construction at
  startup is the plugin's startup hook, and it doubles as the manual trigger.
- `src/pluginmain.cpp` — `DZ_PLUGIN_*` definition and class GUID.

### Releasing

Releases are **built locally** — the Daz SDKs are store downloads that cannot
exist on CI runners, so GitHub Actions only guards the parser tests. To cut a
release:

1. Bump the version in `src/version.h` and commit.
2. Run `.\release.ps1` (parameters default to this machine's SDK/Qt paths;
   add `-Draft` for a draft release).

The script builds both variants, runs the tests, tags `v<version>`, pushes
the tag, and publishes a GitHub release carrying
`dth-character-studio-runner-<version>-ds6.zip` and `dth-character-studio-runner-<version>-ds4.zip`.
It refuses to run on a dirty tree or an already-released version.

### Caveats

- The Daz Studio 6 SDK is **beta** — a rebuild against the final SDK may be
  required when it goes GA.
- Generated ROM scripts report hard failures with a modal `MessageBox`, which
  pauses an unattended batch until dismissed (acknowledged in the spec; a
  future studio runtime suppresses dialogs on bulk runs).
