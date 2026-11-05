# Renderer performance regression

Fractional font metrics exposed an integer/fraction comparison in ghostty-web:
canvas backing dimensions are integers, but the renderer compared them against
unrounded calculated sizes. A stable canvas was reset and fully repainted on
every animation frame, even with no output.

The ttyd source patch now rounds backing sizes consistently, decodes a viewport
only once per paint, skips plain-space foreground draws, and redraws cursors,
selections, and scrollback only when their state or content changes. Background
and cursor rectangles share physical-pixel boundaries; this also prevents seams
and repeated alpha blending at fractional display scales. Glyph positions retain
their fractional spacing.

A local headless Chromium comparison used a 1440×900 viewport, display scale 1,
`fontWidth=0.9`, and a Bash session. Typing sends the same 78 characters with a
requested 20ms delay per character. The overloaded baseline took longer to
process those keystrokes.

| Workload | Main-thread busy / elapsed | Canvas resets | Glyph draws |
| --- | --- | ---: | ---: |
| Idle, before | 1608 / 1609 ms | 40 | 415400 |
| Idle, after | 56 / 1613 ms | 0 | 0 |
| Typing, before | 6400 / 6407 ms | 158 | 1640830 |
| Typing, after | 360 / 1797 ms | 0 | 3783 |

These are measurements from one machine, not universal CPU budgets. Chromium's
`TaskDuration` measures main-thread busy time; it does not measure total browser,
GPU-process, or server CPU. The renderer retains its animation-frame loop, so
idle main-thread work is reduced rather than eliminated.

Run `yarn test:renderer --profile` after building the frontend and C executable.
The suite samples V8 during typing and reports each case as JSON. It checks
normal, fractional, and high-DPI scales, font and window resizing, actual mouse
selection, cursor blinking and styles, ANSI/Unicode, decorated spaces, paste,
scrollback, alternate screens, and frozen/resumed page lifecycle.

Regression assertions require zero spontaneous canvas resets, zero row/glyph
repaints in unchanged states, and at most one viewport decode per paint. For
visual correctness, incremental paints must match a forced full repaint byte
for byte. Failures save canvas PNGs to `/tmp/ttyd-renderer-tests` (override with
`TTYD_TEST_ARTIFACTS`). `TTYD_TEST_SCALES=1.25` isolates one display scale for
investigation. CI runs the full matrix and uploads logs and failure images.
