<!-- Model-output: Claude Opus 5.5 -->

# Review of `prime` (our commits since upstream 6696ab14)

Date: 2026-09-25. Scope: `6696ab14..cd578e8e`, i.e. the 81 commits on top of
upstream `origin/master`. The 19 upstream commits below them were not
reviewed, except for how they interact with ours.

How this was checked:

- Code reading, split into four areas: display buffer / DC state,
  self-drawn graphics, text runs / width caches, hardening / input. Each
  finding below was re-checked against the code before it went in here.
- Compile checks. There is no Cygwin toolchain here, so this used MinGW
  gcc 15.3 with real Cygwin 3.6.10 + w32api 14 headers. The only warnings
  are the `long` 32-vs-64-bit cast warnings, which are an artifact of this
  setup; upstream shows the same ones.
- Nothing was run. There is no Windows here.

## Summary

- **The rebase onto 6696ab14 is fine.** `git range-diff` against the
  pre-rebase head (52cfdd36) shows only context changes, such as upstream's
  `ime_open` → `ime_open_native` rename. The only upstream commit that
  touches `wintext.c` (47b3a510, IME cursor) doesn't interact with ours.
  The upstream `debug_term_paint_timing` code still compiles.
- **The tip compiles cleanly**: `-O3`, with and without `-DNDEBUG`, and with
  every `debug_*` macro in the changed files, except the one in finding 12.
- **Nine commits don't compile on their own** (see "History" below).
- **No crash in a default configuration was found.** The most visible
  problem is finding 1: box-drawing corners are missing pixels.

## Findings, most important first

### 1. Box-drawing corners and junctions are missing pixels — medium-high, confirmed

`src/wintext.c:5349` (`boxline()`). Introduced by df14dc4f, reverted by
11c1d2da, reintroduced by cace626f.

Upstream drew solid straight strokes with a `PS_ENDCAP_SQUARE` geometric pen,
out and back again, so each stroke covered both endpoints plus w/2 beyond
them. The rectangle fast path widens a stroke across its direction but does
not lengthen it, and the far end is exclusive. Effects:

- At line width 1 (default for cells under ~27 px tall), `┘` loses the corner
  pixel (mid,mid). The same happens for `╛ ╜ ╝` and the inner corners of
  `╬ ╣ ╩`.
- Heavy strokes are always at least 3 px wide, so `┏ ┓ ┗ ┛` get 1–4 px
  notches even at the smallest sizes.
- At width ≥ 2 (HiDPI, or larger fonts), every light corner `┌ ┐ └ ┘ ╔ …`
  gets a notch.
- Mixed light/heavy junctions get gaps, and the half-lines `╴╵╶╷` get
  shorter.

cace626f accepts this as "the pre-existing endpoint rasterization
differences". They aren't pre-existing in upstream, which used rectangles
only for dashed segments. 11c1d2da also understates the problem ("strokes
wider than one pixel"). So the trade-off was made based on a wrong
description.

**Fix that keeps the fast path:** for solid segments (`y3 >= -2`), also
extend along the stroke by `w/2` at the start and `w - w/2` at the end, then
clamp the rectangle to `[0,char_width) × [0,char_height)` of the cell.

- The clamp is what upstream's clip did, and it keeps the clip-free path
  safe (see finding 10).
- `hline_fill_run` (388f7542) should get the same treatment, so that batched
  and per-cell output stay identical.
- Worth an eyeball test afterwards with `tools/paint-stress-test` sections
  2 and 9.

### 2. The IME cursor colour fix scales the wrong colour — medium-low, confirmed

`src/wintext.c:6805-6817`, commit 6999f45a.

6999f45a fixed the obvious typos: `max` → `min`, and g/b no longer use r. But
the formula still multiplies `_cc` (the colour of the text under the cursor).
The comment above it says the effective cursor colour `c` should be scaled.

Upstream's `max` bug happened to produce a light cursor. The "fixed" code
can now produce black.

- **Scenario:** dark theme (bg #000000), `IMECursorColour` set, and the
  cursor colour changed to white, either by `OSC 12;#FFFFFF` or in Options
  (`winmain.c:3986`). Then `_cc` is the black background, so the IME cursor
  becomes RGB(0,0,0), which is invisible.
- Startup is unaffected, because `win_reset_colours` sets
  `IME_CURSOR_COLOUR_I` directly afterwards.
- **Fix:** use `c` instead of `_cc` in the three products.

### 3. Background image: padding can become a solid stripe — low-medium, confirmed

Commit e79e8f3b.

- `src/wintext.c:1907-1920`: a stale buffer is seeded by filling the whole
  viewport with the solid bg colour.
- `src/wintext.c:4528-4552`: only default-bg runs repaint the image into the
  padding, and only one `PADDING` past the last column.
- The blit then copies the full client width.

Only when `Background=` is set: after a reseed without a brush reload, the
solid colour overwrites the image that was showing in the padding. Affected
areas are padding next to edge cells with a non-default bg, and the extra
right/bottom margin when the window size isn't an exact multiple of the cell
size (maximized, fullscreen).

- **Triggers for a reseed:** a sixel appears and then goes away, horizontal
  scrolling, or a resize with a tiled image.
- **Fix idea:** when a background is configured, seed with
  `fill_background(paint_buf_dc, &viewport)` and keep the solid fill as the
  fallback.

### 4. Width measurement now uses the bold/italic font — low-medium, plausible

`src/wintext.c:6383`, commit 5d95ae8f (no commit message body).

`act_char_width` renders the glyph and counts it as wide if any pixel is lit
past one cell (`:6585`). With the italic font, the slant and anti-aliasing
fringe can cross that line when the upright glyph doesn't.

- **Affected characters:** non-letter, non-ASCII ones such as `§ ¶ ° ± × ÷ “ ” € —`.
- **Scenario A:** in a CJK-wide locale, italic `§` stops being expanded while
  regular `§` still is.
- **Scenario B:** with the default CharNarrowing, italic/bold punctuation
  picks up `TATTR_OVERHANG`, which means extra run breaks and two-phase
  output.

Upstream measured with `FONT_NORMAL`, probably on purpose. Unless this fixed
a specific observed bug, I'd revert it. The memo key includes bold and
italic either way, so the cache is consistent.

### 5. A late selection release has side effects — low, confirmed

`src/wininput.c:1110-1121`, commits 9f8c7087 and 005fe7fa.

When the `MS_SEL_*` state is stale, the swallowed focus-click release is now
passed to the full `term_mouse_release()`. That function does more than end
the selection:

- With CopyOnSelect (default on), it copies the stale selection to the
  clipboard.
- With ClicksPlaceCursor or readline mouse mode (DECSET 2001), it sends
  arrow keys, moving the shell cursor to the focus-click position.
- In `MS_SEL_CHAR` with nothing selected and OpeningMod held (Ctrl by
  default; any click if it's configured empty), it opens the hyperlink under
  the focus click.

Focus clicks are meant to be inert. A smaller fix would be to set
`term.mouse_state = 0` on this path and call `term_flush()` (to release
output held back during the selection) and `ReleaseCapture()`, without
calling `term_mouse_release()`.

**Side note (plausible only):** `win_mouse_move` detects the lost release
with `GetKeyState(VK_LBUTTON)`. That reads the thread's synchronous key
state, which in principle isn't updated for input that went to another
thread. Windows probably resyncs it when the queue is empty, as Wine does.
If the ghost selection ever comes back, switch to `GetAsyncKeyState` and
account for swapped buttons.

### 6. textprint: a nested job whose open() fails can crash — low, confirmed, unlikely

`src/textprint.c:77-80` and `:116`, commit cf07b4ee.

On `open()` failure, `printer_start_job` now sets `pf = 0` but leaves
`printer` set from an outer job.

- **Scenario:** a `CSI 5 i` job is active, the user starts Print Screen, and
  that second `open()` fails.
- **Result:** `printer_finish_job` runs `for (fi = pf; *fi; …)` with `pf`
  NULL.
- Upstream leaked `pf` instead. Nested jobs were already broken upstream.
- **Fix:** also clear `printer` on that path.

### 7. `--trace`: failing to open the file now aborts startup — low, confirmed

`src/winmain.c:7514-7524`, commit 0ef3e726, plus 57ba0b47, which is correct.

- **Before:** upstream closed stdout and carried on.
- **Now:** `option_error()` calls `exit(1)`.

For a debug option that's defensible, but the commit message doesn't mention
it, and the two new `__()` strings aren't in the .pot.

### 8. Direct DIB writes rely on GDI not batching — low, plausible, deliberate

`src/wintext.c:1928-1932`, `2029-2045`, commit 1124f1dc (which reverted
6d68d6f4).

- The `CreateDIBSection` documentation requires `GdiFlush` between GDI
  drawing and CPU access to the bits. Now there's one flush per frame.
- This is fine as long as DIB-section DCs are never batched, which was
  measured.
- **If they ever are batched:** a bar-cursor fill written directly could be
  overwritten by an opaque `ExtTextOut` still in the queue.
- Separately, the 65987ac5 message calls such an artifact "one-frame, repaired
  by the next update". It isn't: the back buffer persists.

### 9. The ASCII/non-ASCII chunk break is pointless under FontRender=textout, and isn't pixel-neutral — low

`src/term.c:4566`, commits 17cb85a1 and 44dc9421.

- **Unneeded under textout:** every chunk goes through `ExtTextOutW` anyway,
  so the forced break only adds chunks: "café" becomes 2, "naïve" becomes 3.
  - **Possible fix:** gate it on the renderer.
  - **Catch:** `bc` is frozen during merged ASCII runs, so it would then need
    recomputing at each boundary, which generalises the comcom fix at
    `:4548`.
- **Not pixel-neutral:** 17cb85a1 claims it "cannot change a single pixel".
  Chunk boundaries do move, which shifts the phase of dotted/dashed underlines
  (the pen restarts at each chunk's x) and moves where `ETO_CLIPPED` cuts off
  overhang (shadow bold, negative ColSpacing). Arguably harmless.

### 10. Skipping the clip lets bold `║` spill left in narrow cells — low, plausible

`src/wintext.c:5481-5502`, commit 0972b1a9.

- **What:** `boxpower_char_clip_safe` assumes every rectangle stays inside
  its cell. The left stroke of `║ ╬ …` starts at `cw/2 - lw - lw/2`, which
  goes negative once `cw < 3*lw`.
- **Scenario:** bold, large RowSpacing, e.g. cw=8, ch=40, bold lw=4. The
  stroke paints 2 px into the previous cell, and incremental repaints keep
  it there.
- **Fix:** the clamp from finding 1 fixes this too.

A related minor point: under SPD 3 (presentation RTL), the "safe" glyphs now
appear mirrored, while the rest still disappear as before. That's
inconsistent, but it's a rare feature.

### 11. The Ligatures=0 fast path has no font fallback — low, plausible

`src/wintext.c:4393`, commit 2477ac4f.

If the primary font lacks some printable ASCII glyphs (e.g. an icon font as
the main font), `ExtTextOutW` shows missing-glyph boxes, where Uniscribe with
`SSA_FALLBACK` would have picked another font. The commit message's "can only
differ through liga/calt" overstates the equivalence.

**Corner case:** a comcom (U+0E33, U+0EB3, U+0F77, U+0F79) right after merged
ASCII pulls that ASCII back into the shaped chunk. That contradicts the new
man page sentence, but it's very rare.

### 12. `-Ddebug_win_char_width` no longer builds — low, confirmed

`src/wintext.c:6251`, commit 4459d63f.

The debug block's `#define win_char_width xwin_char_width` used to rename the
real implementation. The implementation is now `win_char_width_uncached`, so
the debug self-call there refers to an undeclared `xwin_char_width`.

Also, the "mysterious delay with -O1 or higher" comment in
`win_char_width_uncached` is stale: 282d9fc0 removed the `-O0` special case
for this file, and it's now built at `-O3`.

### Performance-only notes (low)

- **e79e8f3b:** every WM_PAINT that touches the padding sets
  `paint_buf_stale`, which makes the next update repaint and blit everything.
  Painting the same padding into `paint_buf_dc` instead would avoid that.
- **fe25dca4:** the dirty region is a single top..bottom band. A dirty top row
  plus a dirty bottom row (tmux status line plus the cursor) blits almost the
  whole window every frame.
- **9490550b:** buffering is off whenever `term.imgs.first` is set, i.e. when
  any image is still in the scrollback. The man page says "while sixel
  graphics are active".
- **4459d63f:** width 0 is never memoised, but 0 is the normal result for many
  non-BMP characters (e.g. U+1D400 math alphanumerics). Those still cost a
  GetDC/SelectObject/ReleaseDC for every changed cell. The comment "width 0
  signals a failed width enquiry" is inaccurate.

## History / bisectability

Nine commits don't compile on their own. Their fix-ups land later:

| Broken range | Build | Error | Fixed by |
|---|---|---|---|
| e2fa4092 | all | `win_select_font_reset` / `paint_buffered` used before declaration | 6500ba48 |
| cf07b4ee … 7014b279 | all | `GetOEMCP` undeclared in textprint.c | e8805b69 |
| a7822d93 … 2b37103e | release (`-DNDEBUG` + `-Werror`) | unused variable `restored` | 2a299409 |

The branch was rebased today anyway (`gh/prime` is still the old history).
So before the next force-push, consider folding those three fix-ups into
their parents, e.g. mark them `fixup!` and run `git rebase -i --autosquash`.

Commit messages that don't match the code (worth rewording while rebasing):

- **cace626f, 11c1d2da:** see finding 1.
- **17cb85a1:** "pixel-neutral"; see finding 9.
- **d9b48a6c** ("validate font styles before indexing caches"): its only
  change is inside `#ifdef check_font_ranges`, which is never defined. So it
  has no runtime effect.
- **0ef3e726:** the "repair" of the POSIX copy loop is inside
  `#ifdef copyfile_posix`, which is never defined. So that part is dead code.
- **c843c63e:** blames the wrong commit. The unclipped DEC Technical
  regression came from 24ffbb1a.
- **005fe7fa:** refers to "def45a47", the pre-rebase SHA of 9f8c7087.
- **9490550b:** claims emoji drawing restores the world transform. It also
  left GM_ADVANCED set, which 2eadcb99 fixed later.

## Simplification opportunities

- **Perf instrumentation** (1dab7d68, 85041746): about 640 lines of perf.c/h,
  plus ~240 `PERF_*` call sites in term.c/wintext.c hot paths. Most
  `win_text` and fill helpers now carry tick bookkeeping. It's cheap when
  `MINTTY_PERF` is unset, but it's the biggest source of noise in the diff
  against upstream. If the optimisation work is done, consider dropping it or
  cutting it down to a few frame-level counters.
- **Self-drawn GDI caches** (2e84317e):
  - Width-0 solid pens and the trio brush could use the `DC_PEN`/`DC_BRUSH`
    stock objects with `SetDCPenColor`/`SetDCBrushColor`, for identical
    pixels. That removes the brush cache and most pen-cache entries,
    including the colmix colours that cause overflow resets.
  - `setpen(heavypen)` (`:5385`) is unreachable, since every heavy segment is
    straight. Yet `heavypen`, `pen` and `roundpen` are looked up up front on
    every boxpower run (three linear scans of up to 512 entries).
  - The `used` field is redundant: it's always true for entries below
    `*_cache_len`.
- **`selfdraw_clip_rgn`** could become `IntersectClipRect` after `SaveDC`, as
  the curly-underline code already does.
- **Renderer invalidation is done twice** (0ac7b3a1): `font_render` is in
  both `wcw_key` and `font_changed`. The `font_changed` route is the
  necessary one, because it also flushes the charpropcache. Drop
  `font_render` from the key, and fix the comment at `wintext.c:641`, which
  now says the opposite of what the code does.
- **Charpropcache:** it now only matters after memo evictions, so it could
  go. Its `cpfound` "cached but not measured" branch (`:6579`) was already
  dead upstream, because entries are only ever stored with width 1 or 2.
- **Dead code:**
  - the `tek_mode` clause in `paint_buffer_usable` (`:1795`), since both
    callers already branch on Tek mode;
  - the `hdc` parameter and return value of `perf_selfdraw_fillrect`;
  - the `copyfile_posix` block in winmain.c.
- **Stale comments:**
  - the `win_fill_rect` docstring (`:1990`) still says correctness "never
    depends on tracking"; it now depends on `paint_dc_busy`;
  - the `clear_run` comment (`:4509`) still talks about the DC brush;
  - the "we could use FillRect above" comment (`:5377`).
- **Layout left over from 6500ba48:**
  - `paint_dc_busy_push`/`pop` sit in the middle of the `paint_buf_*`
    declarations;
  - the `win_paint_buffer_begin` docstring is separated from its function.

## Housekeeping

- AGENTS.md's `Model-output:` header is present only in `src/wininput.c` and
  `tools/paint-stress-test`. Files with LLM-written code from this branch that
  lack it:
  - `src/wintext.c`, `term.c`, `perf.c`, `perf.h`
  - `child.c`, `config.c`, `printers.c`, `textprint.c`, `winimg.c`,
    `winmain.c`
  - the small header edits in `config.h` and `winpriv.h`

  Most of these commits predate the rule (added 2026-08-30). The authors in
  git are "Claude", "Claude Fable 5", "OpenAI", "Sol" and ChatGPT 5.5
  Pro/Thinking. The model names behind "OpenAI" and "Sol" aren't recorded.
- README.md, first line: "This is an fork" → "a fork".

## Checked and found fine

- **Rebase:** no semantic interaction with the upstream IME rework
  (d7132bb8, 47b3a510), the term_paint timing debug code (dd6aea19,
  0e8344cc), the image scrolling fix (6696ab14, since images bypass
  buffering) or the bg-image leak fix (28bd66e6).
- **DC state tracking:**
  - the font-selection cache (e2fa4092, 570a1b25);
  - `paint_dc_busy` push/pop balance on every goto path;
  - SaveDC/RestoreDC pairing (a7822d93, which also fixes an upstream bug
    where the search-bar clip exclusion was lost);
  - graphics-mode restore (2eadcb99);
  - scrollbar cache invalidation (656bb5f9, a06b5e39).
- **Back-buffer lifetime:** DC deleted before the bitmap; fallback chain DIB →
  compatible bitmap → none; dropped when the option is turned off; minimized
  windows handled.
- **Blit coverage:** full width; a ±1-row margin for double-height and emoji
  spill; tab bar and search bar excluded.
- **Pen/brush caches:** keyed on (ext, style, width, colour); no leaks when
  creation fails; always deselected before deletion; the overflow reset at
  frame start is safe.
- **Width memo** (4459d63f): keyed on everything that affects the result;
  flushed on every font (re)initialisation; the linear-probe invariants hold.
- **Run merging:** removes only bidi-class breaks. Breaks for attributes,
  cursor, selection, search, emoji, combining characters, surrogates and
  narrow cells are unchanged. The comcom fix (2b37103e) restores upstream's
  break decisions.
- **Hardening commits:** all the claimed bugs are real, and the fixes are
  correct on their main paths:
  - 98bfc5b1, 93ce2f34, 7014b279, e8805b69;
  - 32ceaae5: a real NULL dereference when the main image list empties while
    `tempfile_num` > 16;
  - e0e09a1e, c463517e;
  - 0eeef192: confirmed that upstream 716ee6ee made `break` exit the wrong
    loop, so an EOF was never noticed and `hold` spun at 100% CPU;
  - 57ba0b47.
- **Build:** `DisplayBuffering` is wired correctly (default on, option table,
  man page). The Bloom removal is complete (only historical Changelog
  entries remain). `-O3` compiles without new warnings.
