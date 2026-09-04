# Releasing

Dune Weaver Touch ships **four boards from one tree**: `waveshare-7` and
`waveshare-5` (the Waveshare 800×480 panels — same panel config, but only the
7 has non-square pixels), `crowpanel-adv-5` (Elecrow CrowPanel Advance 5.0)
and `crowpanel-7` (the original Elecrow CrowPanel 7.0-HMI, the only 4 MB
board). Every release carries an app image and a bootloader for each — the
bootloader is per board on all four, because even where two envs build
identical bootloader *code* the embedded build timestamp makes the bytes
differ — plus a partition table (`partitions.csv` for the 16 MB boards,
`partitions-4mb.csv` for the 7.0-HMI) and one otadata they all share.

The Waveshare **5** got its own image in v0.1.6-rc3. Before that it was folded
into the 7's board entry, named "ESP32-S3-Touch-LCD-7 or -5", and a 5 owner
installed the 7's 7.6% aspect correction onto square pixels. Renaming that
entry is why `BOARDS` has an **`aka`** field: published manifests still offer
the old name and cannot be edited, so `check_release` accepts any name a board
has shipped under. `build_manifest` never emits one.

The Waveshare **5B** (1024×600, `firmware.bin`) was dropped in v0.1.6-rc2:
the web installer has never offered it, so no release since v0.1.5 was
installable on one. Its envs still build. A 5B in the field now 404s its
update check, which fails safe. Put it back in `BOARDS` and the image returns. Releases are
published as **GitHub Release assets** on `tuanchris/dune-weaver-touch`, and
the `.bin`s and `manifest.json` are also committed to `releases/<tag>/` on
`main`, which is what the web installer reads.

## What a release is, and what checks it

`tools/release_spec.py` is the declaration: the envs, the images, their flash
offsets, and the `Board` → `Installation type` tree the manifest offers.
`tools/build_release.py` builds against it, `tools/check_release.py` verifies
against it. Nothing else should hard-code an offset or an image name.

```sh
python3 tools/check_release.py releases/v0.1.3   # a published release
python3 tools/check_release.py releases/*        # everything on main
```

It fails the release if any image is missing, mis-sized or mis-hashed; if
anything at the app offset is not an app image of this project **at this
version**; if a `.bin` in the directory is not reachable from the manifest; if
a choice installs an image that does not exist, or an image nothing installs;
if a board in `BOARDS` is not offered, or is missing any of the four images it
installs, or has one at the wrong offset; or if two boards somehow got the same
app binary.

A board is required only of releases at or after its **`since`**, which is a
version string (`"v0.1.6-rc2"`), compared with `version_key()` — a published
release cannot grow a board that did not exist when it was built. The key
orders a prerelease *before* its final release, so a board added in rc2 does
not fail the rc1 already on the server. Without that, adding a board mid-cycle
would mean skipping a version number to dodge the comparison. Getting that
wrong is how `check-releases.yml` sat red from v0.1.5 to v0.1.6-rc1: adding the
CrowPanel Advance made v0.1.3 and v0.1.4 fail retroactively, and only the
Release workflow was being watched. **Check both workflows after a tag.**
Genuinely unfixable published releases are listed in `KNOWN_BROKEN` and
reported without failing — v0.1.4 is there, and it is the only one.

`build_release.py` runs it before declaring success, the release workflow
re-runs it on the copy committed to `main`, and `check-releases.yml` runs it
over `releases/*` on every push that touches them.

**This exists because v0.1.3 shipped broken.** The 800×480 image was built,
staged, published and committed — and the manifest, generated before the
two-board change landed, never mentioned it. Every byte a 7 owner needed was
on the server, and the installer still offered them only the 1024×600 build,
which flashes cleanly and boots unreadable. Nothing errored. Releases before
v0.1.3 are genuinely single-board (`MULTI_BOARD_SINCE` in `release_spec.py`),
so the board rules do not apply to them.

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
the board's own app image @ `0x20000` (esp32s3, 80m), or point an ESP Web
Tools installer at `manifest.json`.
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
gitignored. Each board in `BOARDS` names its env and the files that env
contributes; `board_images()` is the single place that says what a board is
made of, and both the builder and the checker read it, so adding a board is a
spec edit rather than a new block in the builder. Flash offsets live beside it
in `tools/release_spec.py` and mirror
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
- **A release whose manifest does not offer every panel is worse than
  invisible** — the version installs, and the wrong half of the owners get an
  unreadable screen. `tools/check_release.py` is the backstop; do not publish
  past it.
- The convenience zip stays an asset only; nothing reads it from the branch,
  and it would double what this repo grows by per release (~1.65 MB as it is).
- `release/` (singular, staging) is gitignored; `releases/` (plural, tracked)
  is not. Don't collapse them.

## What is deliberately not here

One step from the firmware repo's workflow does not apply:

- **The unprefixed `firmware.bin` alias** — for the mobile app's table-OTA
  picker, which has nothing to do with the panel.
