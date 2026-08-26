# STATE — 2026-08-26

## Progress ring rendered as four segments (2026-08-26, photographed by Tuan)

On the board the Now Playing ring showed as four disconnected arcs at 12/3/6/9
o'clock with gaps on the diagonals. Not an arc-drawing bug — an occlusion:
the preview tile is a 300 px **square** whose corners are opaque `th.bg` out
to r=212 (`thr_preview_get`'s `corner`; tiles paint their own corners, they
are never clipped), while the ring sits at r=162..170. `s_disc_img` and the
placeholder dish were created as children of the **arc**, and LVGL draws a
widget's own parts before its children, so the tile's corners painted over
the ring. Geometry matches the photo exactly: the square's edge is 150 from
centre at a cardinal and 212 at a diagonal, so the ring survives only within
±22° of each cardinal — four ~44° segments.

Fixed by putting dish, disc and arc in one 340×340 `stack` container with the
**arc created last** (draws on top); arc bg forced transparent so the disc
shows through. Sizes and styles otherwise unchanged. The stale comment on
`preview_job` claiming the disc "sits directly on the page background" is now
actually true. Verified in the sim by weaving a pattern: one continuous
indicator arc + continuous track. Flashed to the board the same session —
boot clean, 75 s soak with a live table, no fault — but the ring itself has
only been confirmed by eye in the sim, so give the panel a look.

Note for anyone reading a sim screenshot: the tile's corners look very
slightly off-background there (16,16,16 vs 23,19,16). That is the ARGB8888
`lv_snapshot_take` path, not a real mismatch — LVGL's `lv_color_to_u16()`
truncates exactly like `ui_rgb565()`, so on the RGB565 panel the corners are
bit-identical to `th.bg`.

## Header table switcher: the chevron was decorative (2026-08-26, found by Tuan)

Tuan: "nothing happens when clicking the drop down in header to select
different table". Correct — `ui_page_header()` drew the dot, the name and a
`TH_ICON_EXPAND_MORE` chevron with a comment claiming "the name is tappable
(switch-table popup), like the reference", but no `LV_OBJ_FLAG_CLICKABLE` and
no event callback were ever attached to any of them. Since the first
increment the chevron has been a picture. The only way to change tables was
the Control page's TABLES ON YOUR NETWORK card.

Now implemented in `ui.c` against `ConnectionStatus.qml`, so every page's
header gets it: dot+name+chevron are one clickable pill (pressed fill
`th.pressed`), a tap drops a `SWITCH TABLE` card under the header listing the
mDNS-discovered tables — name over URL, the connected one accent-bordered
with a check and inert, any other connects with one tap. A fresh
`discovery_scan` runs per open (the reference calls `refreshSerialPorts()`
before `popup.open()`), so the list is never staler than the tap; the last
result is kept across opens so a re-open paints instantly instead of blinking
through "Searching your network for tables...". Tap outside or tap the pill
again to dismiss.

Sizing is the usual 1.5x: rows 84 (ref 56), card 450 wide (ref 300). The card
sits at y=72 and must clear the nav bar at 600-64=536, so the list is capped
at four rows (`TBL_LIST_MAX_H`) and anything beyond gets `ui_page_stepper` —
built once with the popup and hidden when it does not overflow, because the
stepper hangs callbacks holding its own heap state off the *scroller*, so
delete/recreate per rebuild would leave them dangling.

Verified in the UI sim by driving real clicks (cliclick + `sim/shot.py`):
opens, lists 5 fake tables, marks the current one, stepper reaches the 5th,
row tap re-targets `state_connect_url` (watched it move 8080 -> 8081 -> 8080
in the log), outside-tap closes without leaking the tap to the Browse card
underneath. Flashed; the panel boots and runs clean with it, but the popup
has not yet been opened by finger on the board.

### The bug under the bug: a `plain()` container ate every tap

First cut looked right and was completely dead in the middle: taps inside the
card produced *no* LVGL event at all — not the row, not the card, not the
full-screen scrim — while taps on the scrim outside the card worked. Cause:
each row holds a `plain()` column for the name+URL labels, and a bare
`lv_obj` is `LV_OBJ_FLAG_CLICKABLE` by default (`plain()` only clears
SCROLLABLE). That column spans most of the row, so it won hit-testing, had no
handler, and LVGL does not bubble by default — the tap vanished silently.
One `lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE)` fixed it. Now a rule in
CLAUDE.md; worth checking anywhere else a decorative container sits on a
touch target.

## Boot order: the SD fast path was dead (2026-08-26, found while checking)

Every boot logged `E jobs: submit_to(67): jobs_init not called` exactly once
and nobody had chased it. `app_main()` ran `ui_init()` *before* `jobs_init()`,
and `page_browse_create()` calls `start_load(false)` when
`sd_catalog_present()` — so that submit hit a null queue, `start_load` reset
`s_loading` and, being `user_initiated=false`, said nothing. The card load
then only happened later via `on_state_changed` once a table connected, which
defeats the entire point of the card ("makes Browse independent of the
table"): a card-equipped panel with no reachable table showed an empty Browse
forever.

Fixed by moving `jobs_init` + `fw_client_init` + `thr_preview_init` above the
`ui_init` block in both `src/main.c` and `sim/main_sim.c` (jobs submitted
during `ui_init` simply block on the port lock until it is released; `wifi_init`
and `state_init` stay after, since state's listeners touch widgets). Sim log
now shows `loaded 1232 patterns (SD manifest)` *before* `state: connected`,
and the error line is gone.

Confirmed on the board after flashing: `jobs: workers up` at 1.3 s, the 1232
patterns off the card at 4.0 s — ahead of WiFi association (5.6 s) and the
table connect (7.8 s) — `dune-weaver-touch up` at 4.4 s, and no `submit_to`
error anywhere in the boot chain. Browse is now populated before the panel
has a network at all, which is what the card was for.

## Browse page fill: 2.0 s -> 0.4 s, MEASURED on the board (2026-08-26)

Tuan, after the 4-bit mask change: "did pattern preview run faster? i feel like
it's the same... about 3s per 8 patterns". He was right, and the reason is a
lesson worth keeping: **the mask change made the part that was never the
bottleneck 4x faster.**

Instrumented `load_tile` with `-DPV_DEBUG_TIMING` and flashed. Per 182 px tile:

| stage | before batching |
|---|---|
| SD read (45,000 B) | 27.7 ms |
| expand nibbles | 5.0 ms |
| resample 300 -> 182 | 10.6 ms |
| composite | 9.3 ms |
| **load_tile total** | **52 ms** |
| **gap between consecutive tiles** | **~250 ms** |

load_tile was 52 ms of a 250 ms cycle — **~200 ms per tile was spent outside
the loader entirely**, on the full-screen redraw each attach triggers. That is
worse than the 41.5 ms VSYNC period, because the software render of the whole
widget tree into the PSRAM framebuffer costs more than one frame on top of the
wait.

Fix: `page_browse` loads a PAGE per job and attaches under a SINGLE
`lvgl_port_lock`, so LVGL coalesces all 8 invalidations into one refresh.
Measured after, same instrumentation:

    gaps (ms): [206, 47, 49, 46, 47, 47, 46, 47]   mean 47
    load_tile mean 46 ms — the gap now IS the load, nothing else

**A full page went ~2.0 s -> ~0.4 s (~5x).** Slow cards still show progress:
the batch flushes whatever is ready every `PREVIEW_FLUSH_MS` (400) instead of
leaving the page blank until the last tile lands.

Two things this cost that are worth knowing:

- **`ESP_LOGD` is compiled out on device** (`CONFIG_LOG_MAXIMUM_LEVEL=3`), so
  every per-stage timer in thr_preview only ever ran in the sim. That is how a
  200 ms/tile cost hid through an entire optimisation pass. `-DPV_DEBUG_TIMING`
  (platformio.ini, commented out) promotes them to INFO.
- The slow jobs lane is now held ~0.4 s per page instead of ~50 ms per tile.
  Fine today — discovery is user-initiated and takes 3 s anyway, and transport
  uses the fast lane — but it is the reason to keep batches page-sized.

### Prefetch of page N+1 (same day, MEASURED on the board)

Once the visible page is complete, `preview_tick` warms page N+1 into the RAM
LRU: `PV_KIND_PREFETCH` loads each tile and releases it immediately, leaving it
cached at refs == 0 with a fresh stamp. No widgets, so no lock and no redraw at
all. The next real load finds it in RAM, so a Next tap costs one redraw.

Residency does NOT grow: the LRU was already filling all 24 slots as you
browsed, and prefetch only fills them with tiles you are about to want. The
soak (`-DUI_DEBUG_PREVIEW_SCROLL`, steps a page every 1.5 s) confirms it:

    page 0: 0/8   <- cold start
    page 1: 0/8   <- cold start
    page 2..27: 8/8 tiles resident (~517 KB)   every single step

**26 of 28 pages were already fully loaded when stepped onto.** Resident tile
memory flat at 517 KB across the whole run, internal heap 131 KB, PSRAM 1.92 MB
free and stable — the LRU settles at 24 x 66 KB = 1.58 MB and stays there.

Gating that matters: visible cards always win the jobs lane
(`submit_page_previews()` first, prefetch only if it returns false), and
`s_prefetched_for` is reset in `rebuild_grid` so paging re-aims at the new N+1.

Still on the table: fusing expand/resample/composite into one pass over PSRAM
(24 ms of the 46 is CPU, and three separate passes over PSRAM is most of it).
The first 300 px tile also costs 118 ms building its dish base — one-time per
(size, corner), but it lands on the first detail-overlay open.

## Previews are now ONE folder of 4-bit alpha masks (2026-08-26, Tuan's call)

Superseded the 160 px re-prep below within the hour. Tuan: "replace previews
folder with @160. The touch app should only look for previews." Collapsing to
one folder is right, but `previews/` fed three widgets at two sizes — the
Browse grid at 160, and Now Playing (300) + the Browse detail overlay
(300→480). A 160-only folder made those two upscale, and the detail overlay
went from resolved line art to a smudge. So the folder collapsed to ONE
**300 px 4-bit alpha mask** instead, which is smaller than either old layout
and costs no quality anywhere:

| layout | per tile | card |
|---|---|---|
| 300 RGB565 master only (this morning) | 180,000 B | 201 MB |
| 300 master + 160 RGB565 (midday) | 180,000 + 51,200 B | 274 MB |
| **300 4-bit mask (now)** | **45,000 B** | **55 MB** |

- **A tile is coverage only.** No colour, no dish, no ring. The panel builds
  the dish from `th.preview_dish/ring/sand` (new theme tokens) and composites
  the mask through it. Retheming is now a cache flush — `thr_preview_clear_ram()`
  drops the cached dish bases — not a card re-prep. That closes most of
  backlog item 3; day-mode preview colours are invented and UNREVIEWED.
- **Resampling got cheaper, not more expensive.** The grid's 300→160 runs on
  ONE 8-bit channel (~3 ms) where the old RGB565 path unpacked three (~20 ms).
  Net effect: ~40 ms/tile, *better* than the 45 the 160 px native tile gave.
- **Compositing at target size beats downscaling a composited image.** Thin
  strokes keep their intensity; measured mean brightness 36.2 → 37.6 against
  the old path, and the grid visibly gained contrast.
- The dish rim is antialiased on device (1 px ramp, `base_tile_get`). The
  first cut used a hard-edged 3-class map and the rim differed from the
  supersampled tool render by up to 32/255 along the whole circumference —
  a jagged circle. Fixed by caching an antialiased RGB565 base per
  (size, corner) and lerping toward sand; residual vs the old tool-baked
  master is max 24 / mean 1.34, and that residual is LANCZOS-vs-linear ramp
  profile, not a defect.
- **The file length is the entire format check.** No header, no version. An
  old card logs one warning naming the expected length instead of showing
  1200 silent empty dishes.

Verified: sim builds clean; 1232 patterns load; a page of 8 composites with
no errors; `DWT_SIM_PREVIEW_SELFTEST=<pattern.thr>` (new, sim/main_sim.c +
sim/check_tiles.py) exercises BOTH sizes including the 300 px path that is
otherwise only reachable by tapping a card; firmware composite diffed against
the tool-baked master at 300 and against the old downscale path at 160.
Card re-prepped: 1167 tiles × 45,000 B, `previews@160/` removed.
**NOT yet run on the board.**

## Card re-prepped with native 160 px tiles — SUPERSEDED same day, see above

The `previews@160/` re-prep that every earlier session listed as "still owed"
has been run against Tuan's card:

    python tools/make_pattern_sd.py --out /Volumes/PATTERNS \
        --patterns /Volumes/PATTERNS/patterns \
        --from-bundle <local copy of /Volumes/PATTERNS/patterns/previews>

Result: **1167 tiles in `previews@160/`, every one exactly 51,200 B**
(160×160×2), 73 MB; 0 rendered from `.thr` (all composited from the Pattern
Manager masks), 0 without a source; 65 "already present" = the basename
collisions (1232 manifest − 1167 unique). `previews/` masters and
patterns.json are untouched. Verified by decoding four tiles back to RGB and
diffing against the master downscaled: visually identical, corner is exactly
`#171310` round-tripped through RGB565 = (16,16,16), per-tile mean within
0.5 of the master. **Not yet run on the board** — that is the next step, and
it should take Browse's per-tile cost from ~225 ms to ~55 ms.

Two things learned doing it:

- Copy the shard zips off the card first. The render loop reads masks and
  writes tiles; pointing both at the same USB SD reader makes them fight.
- `make_pattern_sd.py` reopened a shard zip **per pattern** — 1167 reopens,
  each re-reading a central directory, off the card. `load_bundle` now
  returns its open `ZipFile` handles and the loop reuses them (8 shards, held
  for the run, closed at the end). This is why the full pass is ~65 s of CPU.

## Preview load speed: where the time actually goes (analysis, 2026-08-26)

Answering "can we make previews load even faster?". The card re-prep above is
lever 1 of 3; the other two are NOT built. The finding that matters:

**The panel runs at 24 Hz and every tile attach costs a whole frame.**
h_total = 1024+145+170+30 = 1369, v_total = 600+23+12+2 = 637 → 872,053
clocks/frame at 21 MHz PCLK = **24.08 Hz, 41.5 ms/frame**. With
`full_refresh = true` (display.c:173) *any* invalidation sets `inv_areas[0]`
to the whole screen (`lv_refr.c:314-316`), so LVGL re-renders all 614,400 px,
and `avoid_tearing` makes the swap wait for VSYNC. The loader attaches **one
tile per lock acquisition** (page_browse.c:443-495), so an 8-card page costs
8 full-screen redraws. They cannot overlap the SD reads either:
`lvgl_port_lock` is the same recursive mutex `lvgl_port_task` holds around
`lv_timer_handler` (esp_lvgl_port.c:226,242), so it is read → render → read →
render, strictly serial.

| | per tile | 8-card page |
|---|---|---|
| Card this morning (300 RGB565 masters only) | 225 + 41.5 | ~2.1 s |
| 300 master + native 160 RGB565 tile | 45 + 41.5 | ~0.7 s |
| **Card as it is now (300 4-bit mask)** | ~40 + 41.5 | **~0.65 s** |
| + batch a page's attaches under one lock | ~40 | ~0.36 s |
| + pack the tiles into one indexed blob | ~30 | ~0.28 s |
| + prefetch page N+1 while idle | — | ~0.05 s on a Next tap |

Not yet built, in order of value per unit of risk:

1. **Batch the page.** One job loads all 8 tiles (no lock held), then attaches
   them under a single `lvgl_port_lock` → LVGL coalesces the invalidations
   into one refresh. ~300 ms/page, firmware-only, sim-verifiable. Watch the
   slow path: if a read is slow, flush what is ready every ~200 ms rather
   than leaving the page blank for seconds.
2. **Prefetch page N+1** into the RAM LRU when idle. The LRU is 24 slots =
   1.2 MB at 160 px, so current + next + previous fits exactly, and pages
   already stay cached on release (`thr_preview_release` only decrements).
   Makes a Next tap cost one refresh.
3. DONE 2026-08-26: 4-bit alpha tiles (see the top of this file). The
   **packed index** half is still open: one blob with an offset table would
   remove the per-file `open()` in a 1167-entry FAT directory, measured
   1–15 ms each and now a real share of a ~40 ms tile. Settle it before
   asking users to re-prep, and fold it into PORTING_NOTES §7b so the web
   Pattern Manager emits it directly.

**Do NOT raise the SD clock again.** 1319 KB/s at 40 MHz is 10.5 of 40 Mbit —
26% bus utilisation. The limiter is per-transaction overhead plus sdspi's
software CRC16, not the clock, which is why 20 → 40 MHz bought 1.3× and not
2×. Reading *fewer bytes* is the lever that works.

## Text field artifacts (2026-08-26)

The search pill showed a vertical bar at its right edge. Two separate causes,
both fixed:

1. **A scrollbar.** LVGL textareas default to `LV_SCROLLBAR_MODE_AUTO`, and
   shrinking the field to 48 px left its content box (48 − 2×12 = 24) shorter
   than the line height (~27), so it overflowed and sprouted one. `pad_ver`
   is now 10 (content 28, and the line sits centred), and all three textareas
   — Browse search, Control's shared field, the Playlists name field — are
   explicitly `LV_SCROLLBAR_MODE_OFF`. Nothing in this UI scrolls.
2. **The field was stuck FOCUSED**, wearing its accent ring and a blinking
   cursor with the keyboard hidden. `lv_keyboard_set_textarea()` focuses the
   textarea it binds, and page_browse binds at construction and then hides
   the keyboard. Now clears LV_STATE_FOCUSED afterwards, and the cursor is
   styled transparent except in LV_STATE_FOCUSED.

## Grid centred, bars slimmer, and a stepper dead-stop fixed (2026-08-26)

- **Bars shrunk again**: TH_HEADER_HEIGHT 72 → **60**, TH_NAV_HEIGHT 72 →
  **64**. Header controls came down to 48 to clear the 60 px bar; the nav's
  icon+label stack (33 + 4 + 23 = 60) fits 64. Content area is now 476 px.
- **Browse grid is CENTRED on the screen**: the pager column on the right is
  mirrored by an empty gutter of the same width on the left, and the grid
  between them uses `LV_FLEX_ALIGN_CENTER` on the main axis so a row centres
  itself even when the cards don't divide the width exactly. Measured:
  cards span 121..902, centre 511 against a screen centre of 512.
- `CARD_W` 182 (was 200). **The earlier value overflowed**: I sized the cards
  against the body width and forgot the grid's own `pad_hor`, so only three
  fit per row and a page of 8 wrapped to three rows, spilling under the nav.
  Budget is `1024 − 2×PAGER_W − 2×TH_SPACE_LG ≥ 4×CARD_W + 3×TH_SPACE_MD`.

### Stepper dead-stop on long lists (found by Tuan, fixed)

With 8 discovered tables the Control stepper stalled: it walked 0 → 216 →
504 and then refused to move, leaving 1161 px unreachable. Cause was the
SNAP_INSET I had just added — the element sitting at the top edge has offset
`cur + SNAP_INSET`, so a search for "the next element with offset > cur"
re-selected THAT element and computed a target of exactly `cur`. Zero delta,
dead stop. Now the search anchors on `cur + SNAP_INSET`, with a `target ==
cur` backstop that forces a plain viewport step, and for elements taller than
the viewport the step is capped at the next element's top so it never scrolls
past it. Verified: 0 → 216 → 504 → 980 → 1448 → 1665 (bottom reached).

`-DUI_DEBUG_STEP -DUI_DEBUG_STEP_TAB=UI_TAB_<x>` now drives a stepper
hands-free and logs scroll_y/top/bottom each tick — that is what caught this.

## Stepper snaps to elements; buttons got discs; table discovery gaps closed

- **Snap to element boundaries.** Tuan hit a card sliced in half at the top
  of Control. `ui_page_stepper` no longer steps a raw viewport: going down it
  lands on the last element that still starts inside the current view (so the
  first clipped card becomes the first whole one — advances as far as
  possible without skipping), going up it mirrors that. An element taller
  than the viewport has nothing to snap to and falls back to a plain step.
- Offsets are measured against the scroller's **bounding box, not its content
  box**: LVGL clips children to the box, so anything scrolled into the
  padding band is still drawn, and measuring against the content box left a
  sliver of the previous card peeking in above the snapped one. The target
  also backs off by TH_SPACE_XS — smaller than the inter-card gap, so the
  previous element stays fully off-screen while the snapped one is not
  jammed against the edge.
- All step/page arrows now sit on a filled `th.card` disc with a border,
  in both `ui_page_stepper` and Browse's own pager (Browse pages rather than
  scrolls so it can't share the helper, but it must look identical).
- **Browse made symmetric**: `PAGER_W` = touch target + 2×TH_SPACE_LG and
  `CARD_W` 200, so the arrow disc sits TH_SPACE_LG from the right edge
  exactly as the first card sits from the left. Measured on screen:
  content 24..877, arrow 928..999, both margins 24.

### Table selection: it worked, but looked broken (fixed)

Tuan asked. The path was intact — Refresh → `discovery_job` →
`discovery_scan` → `rebuild_table_list` → a Connect button per row →
`state_connect_url()`. Two things made it look dead, both now fixed:

1. **Discovery only ever ran on a Refresh tap.** No scan at startup, so the
   card sat on "No tables found" until you thought to press it. Now the first
   scan fires on the WiFi-connected event (`s_auto_scanned`, once).
2. **The connected table is filtered out of the list** (it is already shown
   in the card above). On a one-table network that always left the list empty
   under the text "No tables found. Tap Refresh…", which reads as a discovery
   failure. The empty state now distinguishes the two cases: it says "That's
   every table on your network - you're connected to it." when the scan DID
   find something and the only thing it found was the current table.

## Dragging removed from the WHOLE UI; every overflow is stepped (2026-08-26)

Tuan: "do the button navigation for all scrollable pages and disable
scrolling everywhere." Done, via one reusable helper rather than five
bespoke pagers.

`ui_page_stepper(parent, scroller)` in ui.c: keeps `LV_OBJ_FLAG_SCROLLABLE`
(lv_obj_scroll_by needs the scroll bounds) but sets `LV_DIR_NONE`, so the
indev finds no scroll target while programmatic scrolling still works —
verified against `lv_indev_scroll.c:327,343` (flag check + per-direction
gate) and `lv_obj_scroll.c:310` (`lv_obj_scroll_by` has no flag check). It
builds an Up/Down column beside the content, steps one viewport per tap
(less TH_SPACE_XL so the row you were reading stays as an anchor), dims at
the ends, and re-evaluates on CHILD_CHANGED/SIZE_CHANGED so list rebuilds
are handled. State is one lv_malloc freed on LV_EVENT_DELETE.

Attached to: Control body, BOTH Light columns, all three Playlists regions
(list, detail patterns, detail settings), and the two modal pickers
(Browse's playlist picker, Control's autoplay picker). Browse keeps its own
card-rebuilding pagination — that also bounds tile memory, which a stepper
would not.

Belt and braces: every page's `plain()` now clears `LV_OBJ_FLAG_SCROLLABLE`,
since LVGL sets it on EVERY object by default — without that, any container
whose content overflows stays draggable even with no `set_scroll_dir` call.

Layout fallout, caught in the sim: two steppers cost the Light page ~80 px
and its palette chips started wrapping mid-word. Fixed by narrowing the
stepper column (TH_SPACE_XS padding instead of TH_SPACE_SM), rebalancing the
columns 42/58 → 48/52, and dropping the column padding to TH_SPACE_MD.

**Sim bug found and fixed:** `sim/shim/sim_remap.h` only rewrote `fopen` and
`stat`, so the switch to POSIX `open()` made the sim read the host root and
every preview silently failed. Added `sim_open`. Note the macro must be
declared AFTER `#include <fcntl.h>` or the system's variadic `open()`
declaration fails to parse. Device was unaffected — this was sim-only.

## Preview read speed: ~450 ms → ~133 ms per tile, MEASURED on the board

Tuan reported ~1 s per preview. Instrumented `sd_preview_load` with
`esp_timer` and flashed to measure instead of guessing. Findings, in order of
what actually mattered:

1. **stdio was the bottleneck, not DMA.** `fread` gave 180 KB in ~400 ms
   (450 KB/s on a bus good for 2.5 MB/s). newlib's fread refills through the
   FILE's own small buffer, so FATFS only ever saw ~1 KB reads. Switching to
   POSIX `open`/`read` with a 32 KB chunk → **173 ms (1014 KB/s), 2.3×**.
   A first theory — that a PSRAM destination forces
   `sdmmc_read_sectors` down its single-block path (real: the S3 does not set
   `SOC_SDMMC_PSRAM_DMA_CAPABLE`) — turned out NOT to be the limiter here; the
   internal bounce buffer alone changed nothing. It is kept anyway because it
   is correct and costs 32 KB.
2. **SD clock 20 → 40 MHz** (`SDMMC_FREQ_HIGHSPEED`): 173 → **133 ms
   (1319 KB/s)**. Stress-tested with the paging soak: 60 pages, ~300 reads,
   **zero errors**. SPI-mode data blocks carry a CRC16 the driver verifies, so
   a marginal bus fails loudly (missing tiles), not silently — back off to
   `SDMMC_FREQ_DEFAULT` if tiles ever start failing.
3. `fstat` on the open fd instead of a separate `stat` — one less directory
   lookup (~1-15 ms). Directory scanning was never the problem: stat and open
   measure 1-15 ms even in a 1167-file flat directory.

Full per-tile breakdown on the CURRENT card (300 px masters only, so every
tile takes the fallback): miss 1-4 ms + master read ~200 ms + bilinear
300→160 ~20 ms + corner fill ~0-2 ms ≈ **225 ms**.

**The remaining 4× is on the card, not in the firmware.** A native 160 px
tile is 51 KB against 180 KB and skips the resample entirely: ~55 ms/tile.
→ **DONE**, see the top of this file. Note the "~0.4 s per page" figure this
section originally quoted was optimistic: it counted only the SD reads and
missed the 41.5 ms full-screen refresh each attach triggers.

### Answered while measuring

- **Dual core?** Yes, S3 is 2×240 MHz, but it does not help here: ~200 of the
  225 ms is waiting on one SPI bus. `LV_DRAW_SW_DRAW_UNIT_CNT=2` would
  parallelise LVGL's own drawing (needs `LV_OS_FREERTOS` first —
  lv_draw_sw.c:33 hard-errors otherwise), but since Browse is paged, drawing
  happens once per tap and is not the constraint. Not worth the locking risk.
- **Cheaper format?** Ranked, all needing a card re-prep: (a) native-size
  tiles, 3.5× — do this first; (b) store an 8-bit ALPHA MASK instead of
  RGB565 and composite the dish on device, another 2× (25 KB) and it makes
  tiles theme-independent, which would also retire the `corner` parameter and
  fix day mode; (c) 4-bit alpha, 4× (12.8 KB) — 16 AA levels is ample for
  line art; (d) LZ4/RLE on top — line art compresses well but adds a decoder
  for less than (b)/(c) give. Projected with (c): ~15 ms/tile.

## Browse is now PAGED, not scrolled (2026-08-26, Tuan's call — NOT on hardware)

Tuan's read of the perf work: rather than optimise scrolling, delete it.
Correct for this panel — `full_refresh` + `avoid_tearing` redraw all
1024×600 every frame a drag is in motion, while a static page costs nothing,
so Prev/Next spends one redraw per tap instead of ~30 a second.

- 4 columns × 2 rows = 8 per page. Two rows is the ceiling.
- **Bars shrunk to buy preview size** (Tuan, same session): TH_HEADER_HEIGHT
  90 → 72, TH_NAV_HEIGHT 96 → 72, so the grid gets 600 − 72 − 72 = 456 px
  and the preview goes to **160**. Header controls came down to 56 to fit
  (search pill, refresh, detail back button). Nav still clears its icon +
  label (33 + 6 + 23 = 62 in 72). Measured on screen afterwards: header
  border at y 72, card rows 84–290 and 308–514, nav strip 528–599 — 14 px
  clearance, nothing clipped.
- `CARD_W`/`CARD_H` 206/206, `CARD_PREVIEW_PX` **160**.
- Rows are MEASURED (`page_size_now`), not hardcoded, because the SD-complaint
  banner steals a row's worth of height whenever it is up.
- **Pager is a column down the RIGHT of the grid** (`PAGER_W` 96), not in the
  header — Tuan's call, and it reads better: they page a vertical list, so
  they're LV_SYMBOL_UP/DOWN, and sitting beside the content keeps them under
  the thumb. Header keeps only the "1-8 of 1232" range label; the old
  "N patterns" count label is gone. Arrows dim at the ends. FontAwesome
  symbols avoid a font regeneration.
- Card name labels are pinned to ONE line
  (`lv_font_get_line_height(TH_FONT_CAPTION)`): at 206 px wide a long name
  wrapped to two lines under LONG_MODE_DOTS and pushed the card past CARD_H
  into the nav bar. Caught in the sim, fixed, re-measured.
- Deleted with scrolling: the catch-tap guard (`SCROLL_CATCH_MS`,
  `grid_scrolled`, `card_pressed`), off-screen unloading
  (`card_offscreen_px`, `unload_card_preview`, the keep/prefetch margins),
  and the "Show more" chunking (`GRID_CHUNK`, `make_more_card`,
  `append_cards`). ~120 lines net gone. `-DUI_DEBUG_PREVIEW_SCROLL` now steps
  pages instead of crawling a scroll.
- Consequence for the card: **`--sizes` default is now `300 160`**, matching
  `CARD_PREVIEW_PX`. A 160 tile is 51 KB (vs 180 for the master), so a full
  page is ~410 KB of PSRAM.
- Keyboard key presses now flash `th.accent` + `th.on_accent` text. The old
  `th.pressed` fill is ~9/255 per channel from `th.card` — invisible, and a
  fingertip covers the key, so the confirmation has to read from the edges.
  Style change only; the pressed state itself needs a finger to eyeball.

### Verified on the board 2026-08-26 (FIRST run with a real pattern card)

The card was in the panel's TF slot for this flash, so the SD path finally
ran on hardware: `TF card mounted at /sdcard: 60350 MB` →
`loaded 1232 patterns (SD manifest)` → PSRAM drops ~750 KB as a page's tiles
load → flat afterwards. Internal heap steady at 168 KB, zero `wifi:m f null`,
zero errors, zero panics across several minutes. The old LittleFS preview
region was erased before flashing (`erase_region 0x610000 0x9F0000`); NVS
survived, WiFi reconnected on its own.

**Still owed:** that card only carries `previews/` (300 px). The grid asks
for 160, misses, and falls back to reading the 180 KB master + resampling per
card — it works, but it is the slow path this whole pass exists to avoid.
→ **RESOLVED later the same day**; see "Card re-prepped with native 160 px
tiles" at the top of this file. Still unverified on the board.

## Preview load speed + scroll smoothness pass (2026-08-26, NOT yet on hardware)

Answering "how do we make previews load faster and scrolling smoother". Five
changes, biggest first:

1. **`clip_corner` removed from every preview slot** — the scroll fix. Traced
   `lv_refr.c:187-241`: a clipped object's children are rendered through an
   ARGB8888 layer + `lv_draw_mask_rect` per corner band, and for a CIRCLE
   radius the top and bottom bands cover the whole tile (the middle band
   computes to zero height). So each visible card was pushing ~166 KB of
   32-bit layer traffic through allocate → render → mask → composite EVERY
   FRAME; 8–16 visible cards ≈ 1.3–2.7 MB/frame on top of the 1.2 MB RGB565
   framebuffer. Replaced by tiles that paint their own corners: `corner` is
   now a `thr_preview_get` parameter (RGB565, part of the LRU key) and the
   area outside the dish is filled with it after load. `radius` alone still
   draws the placeholder dish as a circle — that's cheap, it's only child
   clipping that costs. Now Playing turned out never to have been clipped
   (its disc is a sibling of the dish, not a child), which is what proved the
   approach works.
2. **Card ships 204 px tiles** (`previews@<N>/`, `make_pattern_sd.py --sizes`,
   default `300 204`). The grid displays 204, so it was reading the 180 KB
   master and downscaling every card; now it reads 83 KB and skips the
   resample. Master downscale stays as the fallback for older cards.
3. **Loader kicks itself**: `lv_timer_ready()` when a job lands, instead of
   waiting out the 150 ms tick — up to ~2.4 s of pure idle across a screenful.
4. **Prefetch 150 px ahead** of the viewport (was: only once visible), gated
   on `lv_obj_is_visible(s_grid)` so other tabs don't load tiles. Ceiling is
   unchanged — residency is still bounded by the 300 px keep margin.
5. **SD `max_transfer_sz` 4096 → 65536**: a tile was 20–45 DMA transactions.

Also fixed while testing: `make_pattern_sd.py` drew a flat 2 px stroke at
every size, which is 2.5× the §6 spec at 204 and merged adjacent passes into
solid discs. Stroke now scales with tile size.

NOT done deliberately: `LV_DRAW_SW_DRAW_UNIT_CNT` 1 → 2 (dual-core render).
Needs `LV_OS_FREERTOS` first (lv_draw_sw.c:33 hard-errors otherwise), which
changes LVGL's locking model and interacts with esp_lvgl_port's lock. Try
only if #1 doesn't make scrolling smooth enough. SD clock is also still
`SDMMC_FREQ_DEFAULT` (20 MHz); `SDMMC_FREQ_HIGHSPEED` (40) is a one-liner but
signal integrity on this wiring is unknown and a corrupt read would show as
garbage pixels — test it on the bench, not blind.

Sim-verified (build clean, tiles render as circles with no mask, corner
matches the card surface exactly once quantised to RGB565 — the apparent seam
in `sim/shot.py` output is a snapshot artifact, `lv_snapshot_take` re-renders
at ARGB8888 while the real draw buffer is RGB565). Hardware still owes: the
actual frame-rate win, and the first-ever run with a real card in the slot.

## Previews are card-only now: no on-device render, no flash cache
## (2026-08-26, Tuan's direction — NOT yet flashed, board was unplugged)

`thr_preview` is down to RAM LRU → SD tile. Deleted: the .thr rasterizer
(plot_brush/draw_thick_line/fill_disc/draw_pattern/render_tile),
`fetch_and_render` and the fw_client dependency, the LittleFS FS cache, the
`storage` partition (dropped from partitions.csv), and the
`joltwallet/littlefs` component. Firmware is ~41 KB smaller and mounts one
less filesystem. Rendering now lives only in `tools/make_pattern_sd.py`.

Consequences wired through:
- Two error kinds: `ESP_ERR_NOT_FOUND` (card in, no tile for this pattern) is
  PERMANENT → Browse marks the card PV_FAILED at once and Now Playing stops
  retrying, no log spam. `ESP_ERR_INVALID_STATE` (no card) keeps the 10 s
  backoff so a hot-insert still recovers.
- Preview tick 600 ms → 150 ms: the source is a local card now, not a
  30–60 KB/s wire.
- **Off-screen unloading added** — with a working card EVERY grid card gets a
  tile, and 48 × 83 KB ≈ 4 MB would have exhausted PSRAM (~3.3 MB free). Cards
  load when visible, release >300 px outside the viewport. Verified in the
  sim with `-DUI_DEBUG_PREVIEW_SCROLL`: residency plateaus at 16 tiles
  (~1.3 MB) across repeated full-grid passes, no drift, no thrash.
- Old tiles still sit in the device's flash at 0x610000 (now unallocated).
  Wipe on the next flash — this does NOT touch NVS at 0x9000, so WiFi
  credentials survive:
      python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 \
          --port /dev/cu.usbmodem* erase_region 0x610000 0x9F0000
      pio run -t upload

TODO next session: run those two commands, then Browse with the prepared card
in the slot (first real hardware test of the card path — only the no-card
path has ever run on the board).

## Pattern card prepped from a Pattern Manager export (2026-08-26)

Tuan's 64 GB card (32 GB FAT32 "PATTERNS" partition, MBR) came with a
dune-weaver-website Pattern Manager export: `/patterns/*.thr` (full 1232
library) + `/patterns/previews/` (previews.json + 8 shard zips of 512 px
`.thr.webp` files). Those webps are pure ALPHA MASKS (RGB all zero,
full-bleed rho=1) — nicer anti-aliased strokes than our rasterizer.

Decision: do NOT teach the firmware webp/zip (would need libwebp + inflate,
kills the zero-decode design). Instead `make_pattern_sd.py --from-bundle`
(added same day) composites the masks over the firmware dish look into the
existing card contract at card root. Result on card: patterns.json (1232
entries, AppleDouble `._*` junk filtered), previews/ 1167 tiles / 201 MB
(65 manifest entries share a basename → share a tile, mobile semantics),
0 fallback renders, 0 missing. Tile visually verified by decode. Firmware
unchanged; PM export left intact under /patterns/. Card not yet tested in
the panel's TF slot.



Session context snapshot. Durable rules live in `CLAUDE.md`; the protocol/UX
contract lives in `docs/PORTING_NOTES.md`. This file is point-in-time.

## Where things stand

Increment 2 is flashed and stable on the board. Repo is **git-initialized with
zero commits** (initial commit deliberately not made — Tuan reviews first).

### Verified on hardware (2026-08-25)

- Boot chain clean: NVS settings → CH422G/I2C → RGB panel 1024×600 @ 21 MHz →
  GT911 touch (ID 911) → LittleFS `storage` (10 MB) → WiFi STA → both job
  workers → state poll task.
- WiFi provisioning from the Control page worked end-to-end; credentials
  persisted (`The Bears' Wi-Fi Network`), panel got 192.168.68.156.
- mDNS discovery works: found 4 tables — DWG @ .163, DWMAX @ .123,
  DWMP2 @ .153, DWPro @ .115 (multi-table household → auto-connect correctly
  waits for a manual pick).
- 36 automated tab switches with no crash after the LVGL stack fix
  (`-DUI_DEBUG_TAB_CYCLE` soak).

### Crash fixed this session

LVGL task stack overflow (default 7168 B) rendering Control/Playlists/Now
Playing. Symptoms: StoreProhibited in FreeRTOS portasm, backtraces full of
0xa5a5a5a5, double-core panics. Fix: `port_cfg.task_stack = 16384` in
`src/board/display.c` + `FREERTOS_WATCHPOINT_END_OF_STACK`. Two earlier boot
blockers also fixed: XIP-from-PSRAM (flash ops vs bounce-buffer ISR) and LVGL
CLIB malloc (builtin 64 KB pool OOM → `while(1)` assert). All three are
documented as rules in CLAUDE.md.

### NOT yet exercised against a real table

Table pick/connect, status polling against real firmware, pattern browse with
previews (board serves .thr at 30–60 KB/s; first render slow, then FS-cached),
weave + clear modes, playlists CRUD/run, transport, speed, LED page, ball
tracker, password-protected boards (401 path), auto-play read-back. Next
session should start here: pick a table on Control → walk every page.

## BUG (found 2026-08-25, live-tested against a real table): browse grid
## exhausts internal RAM → WiFi starves → red dot + crawling fetches

First real-table session: picking a table made pattern loading crawl and the
header dot go red while requests still (slowly) succeeded. Serial capture
showed the smoking gun: `wifi:m f null` at ~10 Hz (WiFi driver can't allocate
frame buffers) + `mdns_receive: Cannot allocate memory` — **internal** heap
exhaustion (the "free heap: 2.9 MB" in those lines is PSRAM, not internal).

Cause chain: every real table carries ~1000+ patterns (measured: DWG 1031,
DWMAX 1080, DWMP2 1224, DWPro 946). `page_browse.c` `rebuild_grid()` creates
a card per pattern — no virtualization — and with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` every small alloc (LVGL objects,
cJSON nodes during /sand_patterns parse, fw_client's cached path strings ×2
copies) lands in internal RAM. Several thousand sub-4 KB allocations later the
WiFi/lwIP driver can't get buffers → status polls fail → 3-streak →
CONN_LOST → red dot (the dot is truthful; the panel's own networking is
degraded, the table is fine). Grid build also runs under lvgl_port_lock →
seconds of UI freeze. Previews themselves are correctly lazy/visible-only,
but every /sd fetch shares one s_wire_mutex with the poll, at 30–60 KB/s.

FIXED same day (flashed + verified): Browse now builds 48 cards per chunk
behind a "Show more" tile (no more 1000-card internal-RAM bomb), and the SD
card scheme below removes the network preview path entirely when a card is
in. Verified on hardware: DWMP2's 1224 patterns load with no wifi:m-f-null
storm. UPDATE later that day: pressure DID reappear (that verification was
over-optimistic — the margin was <1 KB); root-caused and fixed with
ALWAYSINTERNAL=0, see the internal-RAM section below.

## Pattern SD card scheme (added 2026-08-25, Tuan's direction)

Like dune-weaver-mobile: manifest + pre-rendered previews live on a microSD
the user preps and pops into the panel's TF slot. Contract in PORTING_NOTES
§7a; card prep via tools/make_pattern_sd.py (Pillow render, firmware-identical
look, ~200 MB for the full 1232-pattern library). Browse: SD manifest first
(works offline, loads at page-create), /sand_patterns fallback + complaint
banner when no card; previews RAM → SD → LittleFS → fetch+render. Verified:
in the UI sim end-to-end (1232 patterns from SD manifest, zero network
preview fetches); on hardware only the NO-card path so far (graceful
ESP_ERR_TIMEOUT, banner, table fallback) — a real prepared card in the slot
is still untested. Hot-insert: next Browse refresh remounts (weak
sdcard_remount hook). New board facts: SD SPI MOSI=11 SCK=12 MISO=13, CS =
CH422G EXIO4 active-low held for the session (board.c now tracks the EXIO
latch byte; sdspi runs CS-less).

## Browse scroll: laggy + ghost-selects a card mid-slide (fixed 2026-08-25,
## NOT yet verified on hardware)

Reported on device: scrolling the pattern grid sometimes opened a pattern's
detail even though the gesture was a slide, and scrolling was laggy. Three
causes, three fixes:

1. **Lag**: cards displayed the 300 px master tile in a 204 px slot via
   `LV_IMAGE_ALIGN_STRETCH` — a draw-time LVGL image transform on every
   visible card, every frame, through the clip_corner circle mask, with
   `full_refresh` redrawing all 1024×600 each frame. Fix: `thr_preview_get`
   now derives non-300 sizes from the 300 master (SD tile → FS cache →
   fetch+render, then integer bilinear downscale, FS-cached per size), and
   Browse requests CARD_PREVIEW_PX (204) so the grid blits 1:1. The .thr
   fetch still happens at most once per pattern. Detail overlay keeps the
   300→480 stretch (static image, repaint-only cost).
2. **Ghost select, cause A (sampling)**: the stock esp_lvgl_port read
   callback samples GT911 from inside the LVGL task; during a slow frame a
   flick collapses to "press at A, release at A" → LVGL sees no movement →
   CLICKED on the card. Fix: `src/board/display.c` now runs a dedicated
   10 ms touch-poll task feeding a queue; the LVGL read callback drains the
   backlog with `continue_reading`, so gestures are classified from real
   samples no matter how late they're processed (device-only; the sim's SDL
   indev is untouched).
3. **Ghost select, cause B (catch-tap)**: LVGL by design delivers CLICKED
   when you tap a coasting list to stop it (`lv_indev.c` stops the throw and
   treats it as a fresh press; QML Flickable swallows this). Fix:
   page_browse tracks LV_EVENT_SCROLL ticks on the grid and swallows a
   card/Show-more click whose press landed within 200 ms of scroll motion.

Verified in the UI sim: build clean, grid renders the bilinear 204 tiles
(visually clean line art), `_204.raw` derivatives cached alongside `_300.raw`
in /storage/pv. On hardware (flashed 2026-08-25): boots clean, previews
fetch+render+downscale from the real table, screen sleep engages. Still to
verify by hand on glass: scroll smoothness and flick/catch-tap feel.

## Internal RAM actually exhausted → ALWAYSINTERNAL=0 (fixed + measured
## 2026-08-25)

Flashing the scroll fix re-surfaced the `wifi:m f null` storm in the
no-SD-card + real-table path (DWMP2, 1224 patterns) — continuous at ~10 Hz
(= the AP's beacon interval: EVERY beacon rx buffer alloc failed). A/B
against the pre-fix baseline showed the baseline was also degraded (preview
fetches failing, "table unreachable", ~1 storm line per 30 s) — the earlier
"verified storm-free" note was over-optimistic; the margin was gone either
way, the new ~4 KB touch task just made it loud.

Measured with the new heap heartbeat (10 s ESP_LOGI in main.c): after
"loaded 1224 patterns (table)", **internal free = 743 B, largest block =
108 B**. The whole five-page LVGL widget tree + cJSON parse nodes + every
other sub-4 KB malloc was landing internal via
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`.

Fix: `ALWAYSINTERNAL=0` in sdkconfig.defaults (plain mallocs prefer PSRAM;
WiFi/DMA allocs are caps-explicit and keep the 64 KB internal reserve).
Measured after: **internal free = 169.8 KB, largest 63 KB, dead stable**,
zero storm lines, previews fetching+rendering from the table for the first
time in this scenario. 2×+ full `-DUI_DEBUG_TAB_CYCLE` loops across all five
tabs with the widget tree in PSRAM: no faults, no heap drift. This closes
the "still open: LVGL allocs landing internal" item from the earlier browse
bug. Remember: changing sdkconfig.defaults needs `rm sdkconfig.waveshare-5b`.

## Table simulator (added 2026-08-25)

`tools/table_sim.py` (stdlib-only) mocks the full firmware API + mDNS so the
panel can be exercised without a real table — status/state machine (Run/Hold/
Home), playlist engine with clearing + shuffle + rest pauses, ETag/304 on
`/sand_patterns`, throttled `/sd` (~45 KB/s), `$`-commands incl. `$Bye`
(4 s dead-socket "reboot"), ESP3D upload for playlist CRUD, `--password` for
the 401 path, `--heap-low` for the 30 s poll backoff. Verified via curl:
discovery TXT, run/pause/resume/stop, playlist run/skip/clearing, 304, and
throttle timing all match the contract. Not yet exercised from the panel.

## UI simulator (added 2026-08-25)

`sim/` runs the firmware UI on the Mac — see CLAUDE.md for build/run. The
device sources compile unmodified; only board/, wifi.c, discovery.c are
faked. Verified end-to-end on 2026-08-25: boot → stub discovery → auto-connect
to the table sim → connect edge (18 settings, LED ring, time push) → 1232
patterns + playlists loaded → 1 Hz status polls → throttled preview fetches.
Note the sim showed "loaded 1232 patterns" with no visible stall — the
internal-RAM bug above is device-only, which is exactly why hardware still
gates release.

## Build/debug quick reference

- `pio run -t upload` then serial via `/dev/cu.usbmodem*` (USB CDC; UART0 is
  physically wired to RS485).
- Crash repro hands-free: `-DUI_DEBUG_TAB_CYCLE` in `platformio.ini`
  build_flags (auto-cycles tabs every 4 s with heap telemetry).
- Decode panics:
  `~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/waveshare-5b/firmware.elf <addrs>`
- Changing `sdkconfig.defaults` requires `rm sdkconfig.waveshare-5b` first.

## How this code was built (for archaeology)

Increment 2 was a 12-agent ultracode workflow: 10 parallel implementers
against contract headers (net/, app/, render/, pages), then protocol +
concurrency auditors. Audit fixes applied during integration: preview-cache
refcounting (use-after-free), poll-vs-disconnect generation guard, board-NVS
playlist seeding on connect, settings mutex, jobs fast lane (transport never
queues behind slow fetches), autostart control with read-back, SD-path
convention (`fw_fetch_sd` prepends `/sd`). One workflow agent fabricated its
completion report (fw_client.c was never written) — caught by the auditor,
re-run with proof-of-write required.

## Deferred backlog (increment 3+)

1. DONE 2026-08-25: Outfit + Material Icons Round shipped (`tools/gen_fonts.sh`
   → `src/ui/fonts/outfit_{18,21}.c`, `outfit_sb_{26,36}.c`, each merged with
   the §7 Material subset + LVGL's FontAwesome symbols so LV_SYMBOL_* and the
   keyboard's built-in glyphs keep working). All pages now use `TH_ICON_*`
   Material macros (nav bar matches the reference glyph-for-glyph). Same
   session: themed keyboard (`ui_keyboard_create`, was stock light), circular
   preview clipping (clip_corner on the three preview surfaces), 4-column
   browse grid (CARD_W 230 — matches reference density AND physical size),
   header chevron, search pill w/ magnifier. Verified side-by-side against
   the QML reference running on the Mac against the table sim (PySide6 venv +
   self-screenshot hook in scratchpad; `sim/shot.py` snapshots the LVGL sim).
   Kept deliberately different: speed label says MM/MIN (reference's MM/S is
   the documented mislabel). Also different since 2026-08-25 (Tuan's
   direction): Control page is one full-width card per row (the reference's
   two-column ScrollView is gone).
2. DONE 2026-08-25: screen sleep + touch-to-wake (`src/ui/screen_sleep.c`).
   1 Hz LVGL timer vs `lv_display_get_inactive_time`; sleep = backlight off +
   opaque-black shield on `lv_layer_sys`; wake = backlight on at PRESSED,
   whole gesture swallowed until RELEASED (shield covers everything, so
   GT911's bogus wake coordinates can't hit a widget). Reads the setting live
   — Control-page chips apply immediately. Verified in the sim end-to-end
   (5 s timeout: sleep, wake-on-click with swallow, re-arm); on device the
   flash boots clean but sleep/wake awaits an idle window + a finger — set
   the 30 s chip on Control to test. NOT implemented: the reference's
   general "swallow any first touch after 2 s idle" (only the sleep-wake
   swallow) — add only if bogus taps show up on hardware after short idles.
3. Live night/day retheme (Control switch persists but applies on restart;
   widgets bake colors at creation).
4. DONE/MOOT 2026-08-26: preview cache keying (stale tiles after re-upload) —
   the flash cache is gone; the card is the only copy, so re-prepping the
   card is the update path. Card tiles are still basename-keyed, so two
   different patterns sharing a basename share one tile (mobile does the
   same; 65 of 1232 collide in Tuan's library).
5. Playlist row "<n> patterns" counts: capped at 12 rows, first fetch failure
   aborts the pass (board has no playlist manifest route).
6. Quiet hours ($Sands/*) UI — board-side feature, not in the reference touch
   UI either; needs the TZ push (fw_sync_time currently sends epoch only).
7. OTA partition scheme (single factory app today).

## Environment facts

- Board: Waveshare ESP32-S3-Touch-LCD-5B on USB (`/dev/cu.usbmodem*`),
  ESP32-S3 rev v0.2, 16 MB quad flash, 8 MB octal PSRAM.
- Reference app (feature parity target):
  `/Volumes/SSD/projects/dune-weaver-pi/dune-weaver-touch` (PySide6/QML).
- Waveshare demo bundle + wiki extracts were the pinout/timing source; pins in
  `src/board/board.h`.
