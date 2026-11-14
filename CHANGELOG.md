# Changelog

Notable changes to JonBoy. Each set of related commits is tagged: a minor bump
for a feature set, a patch bump for a smaller set. Sections are grouped by month,
with a subsection per released version. The Unreleased section collects entries
as work lands, before the next bump moves them into a version.

## Unreleased

## November 2026

### 0.2.0 (2026-11-13)

First tagged release. JonBoy boots the retail BIOS and runs games from a disc
image. Where the project stands:

- Emulation: an R3000A interpreter with the GTE (the full CPU conformance suite
  passes); a software GPU for flat and textured polygons, rectangles, and VRAM
  transfers, rasterizing on a worker thread by default; SPU audio with 24 ADPCM
  voices, reverb, noise, pitch modulation, sweeps, and CD-DA and XA streaming;
  MDEC full-motion video; and CD-ROM mounting of `.bin`, `.cue`, and `.iso`.
- Input and storage: a DualShock (digital and analog) driven by the keyboard or
  an SDL gamepad, two memory cards, and an invented PSIO-style SD-card cartridge
  with an in-emulation loader menu.
- Interface: a native menu overlay (Escape) for discs, save states, memory cards,
  view, and controller settings, with settings persisted to `jonboy.ini`. The
  window is locked to 4:3, fullscreen pillarboxes or letterboxes, and the scaling
  filter is selectable between linear and nearest.
- Tooling: headless, `--boot`, `--exe`, `--gdb`, and `--bench` run modes; a
  save-state determinism self-test; a `jonboy.1` man page; `make install`
  (`PREFIX`, `DESTDIR`); an unsigned macOS `.app` bundle; and GitHub Actions CI.
