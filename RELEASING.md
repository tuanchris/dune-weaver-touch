# Releasing

Dune Weaver Touch ships one board target — **`waveshare-5b`** (Waveshare
ESP32-S3-Touch-LCD-5B, 16 MB flash, 8 MB PSRAM). Releases are published as
**GitHub Release assets** on `tuanchris/dune-weaver-touch`: the flat `.bin`
images, a `manifest.json` an ESP Web Tools installer can consume, and a
convenience zip.

## Cut a release

Pushing a **`v*`** tag triggers the **Release** workflow
(`.github/workflows/release.yml`), which runs `tools/build_release.py` on a
clean ubuntu runner and publishes the staged assets.

**The release notes are the annotated tag's message body** — write the changelog
into the `git tag -a` annotation (subject line = title, everything after the
blank line = notes). House style: a one-line summary of what this is, bulleted
changes with enough detail to be useful, an honest verification paragraph
(what ran on hardware vs. in the sim), and the install line.

```sh
git tag -a v0.1.1 -F - <<'EOF'
Dune Weaver Touch v0.1.1

One-line summary of the release.

- **Headline change.** What it does and why it matters.
- ...

Verified on hardware: <what actually ran on the board>.

Install: flash `bootloader.bin` @ `0x0`, `partitions.bin` @ `0x8000`, and
`firmware.bin` @ `0x10000` (esp32s3, 16MB, 80m), or point an ESP Web Tools
installer at `manifest.json`.
EOF
git push origin v0.1.1
```

**Then check what actually got published** — the workflow writes an asset table
and the first five lines of the notes into the run summary, or:

```sh
gh release view v0.1.1 --json body -q .body | head -5
```

If the notes read like a *commit* message, the tag was lightweight:
`actions/checkout` leaves the triggering tag as a lightweight ref pointing at
the commit, and `%(contents:body)` on one returns the commit message. The
workflow re-fetches the tag object and checks `git cat-file -t` before trusting
it, but that is the symptom to watch for — fix with
`gh release edit <tag> --notes-file <file>`.

### Pre-releases

A tag with a suffix (`v0.1.1-rc1`, `v0.2.0-beta2`) publishes as a GitHub
**pre-release**, so `/releases/latest` keeps pointing at the last final release.

### Never let two tags share one commit

Cutting a final release at the same commit as its last RC — the obvious move,
since the code is identical — is what broke this in the firmware repo:
`git describe` tie-breaks arbitrarily between the two tags and staged the whole
release under the RC's name. The workflow resolves the tag from the triggering
**ref** (`DW_RELEASE_TAG`) rather than from `describe`, and
`tools/build_release.py` honors that variable, but the safe habit is an empty
commit between an RC and its final tag.

`gh release create` deliberately **fails** if the release already exists. That
failure is the backstop that caught the bug above — don't soften it into an
upsert.

## Build the assets locally

```sh
python3 tools/build_release.py        # stages release/<tag>/
python3 tools/build_release.py -v     # verbose pio output
```

The tag comes from `DW_RELEASE_TAG` if set, else `git describe`. `release/` is
gitignored. Flash offsets live in `IMAGES` at the top of the script and mirror
`.pio/build/waveshare-5b/flasher_args.json` — if the partition table moves,
re-read that file rather than editing the offsets from memory. Note the S3 boots
its bootloader from `0x0`, not the `0x1000` you may remember from the ESP32.

## Bumping the version

The app-descriptor version — what `ota_version()` returns, what the OTA compare
uses, and what the panel prints at boot — comes from **`version.txt`** in the
project root. `CONFIG_APP_PROJECT_VER_FROM_CONFIG` is deliberately unset, so
nothing in sdkconfig overrides it.

**PlatformIO does not track `version.txt` as a build dependency.** It is read at
CMake *configure* time, so editing it and running `pio run` rebuilds nothing and
you get the OLD version in the image — silently. Touching sources or deleting
component object files does not help either; only a reconfigure does:

```sh
echo 0.1.2 > version.txt
rm -f .pio/build/waveshare-5b/CMakeCache.txt
pio run -t upload
```

Then **confirm it on the board**, because this fails quietly and a release whose
binary reports the previous version breaks the OTA compare for everyone on it:

```
I (650) app_init: App version:      0.1.2
I (4783) ota: update server up on :80 (fw=0.1.2 ...)
```

CI is unaffected — `tools/build_release.py` runs on a clean runner, so it always
configures fresh.

## Before you tag

Hardware gates release here; the sim does not reproduce internal-RAM
exhaustion, stack overflows, or flash-cache faults. At minimum:

```sh
pio run -t upload && pio device monitor
```

and confirm a clean boot: table discovery, no errors or warnings, and a flat
`app: heap:` heartbeat (internal free should not drift across several minutes).
Walk the pages you touched on the panel itself.

## The installer reads `releases/<tag>/` off `main`

The Dune Weaver installer has a Touch panel section (duneweaver.com/install →
*Dune Weaver Touch Panel*), and it flashes from
`raw.githubusercontent.com/tuanchris/dune-weaver-touch/main/releases/<tag>/`,
**not** from the GitHub Release assets. That is not a preference: a release
asset download redirects to `release-assets.githubusercontent.com`, which sends
no `Access-Control-Allow-Origin`, so the browser refuses the fetch. So the
workflow's last build step commits `manifest.json` and the three `.bin` images
into the tracked `releases/<tag>/` on the default branch.

- **A release that skips that step is invisible to the installer** — the
  version shows up in the picker (that list comes from the API, which *is*
  CORS-enabled) and then the manifest 404s. If someone reports that, check the
  branch, not the release.
- The convenience zip stays an asset only; nothing reads it from the branch,
  and it would double what this repo grows by per release (~1.65 MB as it is).
- `release/` (singular, staging) is gitignored; `releases/` (plural, tracked)
  is not. Don't collapse them.

## What is deliberately not here

One step from the firmware repo's workflow does not apply:

- **The unprefixed `firmware.bin` alias** — for the mobile app's table-OTA
  picker, which has nothing to do with the panel.
