# RECUDO additions to tsMuxer

This fork adds dual-layer authoring features on top of jaminmc/tsMuxer. All are opt-in;
default behaviour is unchanged.

## New options (mux mode)

| Option | Purpose |
|--------|---------|
| `--disc-size=<bd25\|bd50\|bd100\|bd128 \| bytes>` | Fit-to-disc guard. Aborts **before** muxing if the estimated image won't fit the target disc. |
| `--allow-oversize` | With `--disc-size`, downgrade an over-capacity overrun from a hard error to a warning. |
| `--layer-break-guard=<MB>` | Zero-fill `<MB>` **after** the BD-R/RE DL layer break (the start of layer 1, where discs are defect-prone), plus a fixed 4 MB margin **before** it. No file data lands on those sectors; the crossing file (usually the movie) stays one logically-contiguous UDF file (two extents around the gap) and plays seamlessly via read-ahead. The zone is **asymmetric** on purpose: real-hardware testing showed the layer-1 defect can be **~35 MB** while the tail of layer 0 is clean, so the budget goes where the defect is. **Use 64** (covers ~35 MB with margin). `0` = align without filler. Off when not given. |
| `--layer-break-lbn=<sector>` | Override the layer-break sector (see below). |

## New mode: `--bdmv-to-iso`

```
tsMuxeR --bdmv-to-iso [--layer-break-guard=<MB>] [--layer-break-lbn=<sector>] <BDMV_folder> <out.iso>
```

Wraps an existing (already-decrypted) BDMV folder into a UDF 2.50 BD-ROM ISO
**byte-for-byte** — no re-mux, no re-numbering. Every `.bdjo`, `.jar`, `.clpi`, `.mpls`,
`index.bdmv`, `MovieObject.bdmv` and the `CERTIFICATE/` chain are copied unchanged, so
**BD-J menus and all clip/playlist references stay valid** (the guard band only moves
UDF extents, which the filesystem hides from the player). The largest `.m2ts` is written
first so the main title straddles the layer break and receives the guard band. MakeMKV
helper folders are skipped.

Playback target: software players (VLC / libbluray / Kodi / PowerDVD) are the reliable
environment for a self-authored BD-J disc; some set-top players restrict BD-J on
recordable media.

## IMPORTANT: which number is `--layer-break-lbn`?

`--layer-break-lbn` is the disc's **Layer 0 capacity, in 2048-byte LBA sectors** — i.e.
the sector where the drive physically switches from layer 0 to layer 1. On BD-R/RE DL the
two layers are equal, so:

```
layer-break-lbn = (disc TOTAL sector count) / 2
```

For a standard 50 GB BD DL that is `24,438,784 / 2 = 12,219,392` (= 25 GB per layer),
which is the **default** — so for standard media you usually don't need this option at all.

### The trap (read this)

You must take the TOTAL from the disc's **full formatted capacity**, not from whatever a
generic API hands back. Example seen on a real Verbatim BD-R DL:

| Source | Total sectors | /2 | Correct? |
|--------|---------------|-----|----------|
| ImgBurn "Disc Information → Free Sectors" | **24,438,784** | **12,219,392** | ✅ real break |
| Windows IMAPI `TotalSectorsOnMedia` | 23,652,352 | 11,826,176 | ❌ a partial format-capacity descriptor — **0.8 GB too early** |

Feeding the wrong (smaller) number puts the guard zone ~0.8 GB before the real layer
transition, so live video ends up on the defect-prone sectors — the exact thing the guard
exists to prevent. **Use the disc's full formatted capacity** (ImgBurn "Free Sectors", or
the `READ FORMAT CAPACITIES` "formatted" / max descriptor), then divide by 2.

An orchestrator should read this number from the drive and pass it to `--layer-break-lbn`;
tsMuxeR itself never touches the burner.

## Recommended flow (BD-R/RE DL with menus + guard)

1. Read the target disc's full capacity (e.g. ImgBurn "Free Sectors"); `LBN = total / 2`.
   For a standard 50 GB disc, `LBN = 12,219,392` (the default — skip step in that case).
2. `tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=<LBN> <BDMV_folder> out.iso`
3. Burn `out.iso` with verify. The DL layer break is media-fixed, so the burner switches
   layers at the same LBN and the guard zone lands exactly on it.
