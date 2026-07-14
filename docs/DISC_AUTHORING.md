# Dual-layer and BD-R XL authoring additions to tsMuxeR

This fork adds multi-layer (BD-R/RE DL and BD-R XL) authoring features on top of
jaminmc/tsMuxer. All of them are opt-in; the default behaviour is unchanged.

## New options (mux mode)

| Option | Purpose |
|--------|---------|
| `--disc-size=<bd25\|bd50\|bd100\|bd128 \| bytes>` | Fit-to-disc guard. Aborts before muxing if the estimated image will not fit the target disc. |
| `--allow-oversize` | With `--disc-size`, turn an over-capacity overrun from a hard error into a warning. |
| `--layer-break-guard=<MB>` | Zero-fill `<MB>` after each layer break (the start of the next layer, where discs are defect-prone), plus a fixed 4 MB margin before it. No file data lands on those sectors; the file that crosses the break (usually the movie) stays one logically contiguous UDF file, two extents around the gap, and plays seamlessly through read-ahead. The zone is asymmetric on purpose: real-hardware testing showed the next-layer defect can reach about 35 MB while the tail of the previous layer is clean, so the budget goes where the defect is. Use `64` (covers about 35 MB with margin). `0` aligns without filler. Off when not given. |
| `--layer-break-guard-before=<MB>` | Optional. Size the zone BEFORE each break on its own, instead of the default small 4 MB margin. The default is asymmetric because the measured defect sits at the start of the next layer, but some media are also weak just before the break, so this lets you pad both sides. Leave unset to keep the asymmetric default. |
| `--layer-break-lbn=<sector[,sector...]>` | Set the layer break sector(s). One value for BD-R/RE DL, two for 100 GB BD-R XL, three for 128 GB BD-R XL (see below). |

## New mode: `--bdmv-to-iso`

```
tsMuxeR --bdmv-to-iso [--layer-break-guard=<MB>] [--layer-break-lbn=<sector[,sector...]>] <BDMV_folder> <out.iso>
```

Wraps an existing, unprotected BDMV folder into a UDF 2.50 BD-ROM ISO byte-for-byte, with
no re-mux and no re-numbering. Every `.bdjo`, `.jar`, `.clpi`, `.mpls`, `index.bdmv`,
`MovieObject.bdmv` and the `CERTIFICATE/` chain is copied unchanged, so BD-J menus and all
clip and playlist references stay valid (the guard band only moves UDF extents, which the
filesystem hides from the player). The largest `.m2ts` is written first so the main title
straddles the layer break and receives the guard band. MakeMKV helper folders are skipped.

Playback target: software players (VLC, libbluray, Kodi, PowerDVD) are the reliable
environment for a self-authored BD-J disc. Some set-top players restrict BD-J on recordable
media.

## Working from an existing ISO

tsMuxeR reads a BDMV folder, not an ISO image, and it never edits an ISO in place. You do not
have to unpack the ISO first, though. Mount it in a virtual drive so it shows up as a drive
letter, then point `--bdmv-to-iso` at that drive and let tsMuxeR read the disc structure
straight from the mounted image:

- Windows 8, 10 and 11: double-click the `.iso` to mount it (for example as `E:`).
- Windows 7: install a free virtual-drive tool (WinCDEmu or Virtual CloneDrive), then mount.

```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=<breaks> E:\ out.iso
```

tsMuxeR reads the files from the mounted drive and writes a new guarded ISO in one pass, byte
for byte, with no re-mux. This only works with unencrypted material: a mounted retail image
that still carries AACS is not readable as plain files. You only need this step when you are
burning to dual-layer or BD-R XL media and want the layer-break guard. A single-layer image
that already plays can be burned directly.

## IMPORTANT: which numbers are `--layer-break-lbn`?

`--layer-break-lbn` is the sector where the drive physically switches from one layer to the
next, in 2048-byte LBA sectors. A recordable BD splits its user-data capacity equally across
its layers, so the break sectors are simple fractions of the disc's total sector count:

```
BD-R/RE DL  (2 layers): total / 2
BD-R XL 100 (3 layers): total / 3  and  total * 2 / 3
BD-R XL 128 (4 layers): total / 4,  total * 2 / 4,  total * 3 / 4
```

For a standard 50 GB BD DL that is `24,438,784 / 2 = 12,219,392` (25 GB per layer). For a
100 GB BD-R XL with a total of `47,305,728`, the two breaks are `15,768,576` and
`31,537,152`.

### The trap (read this)

Take the total from the disc's full formatted capacity, not from whatever a generic API
hands back. Example seen on a real Verbatim BD-R DL:

| Source | Total sectors | / 2 | Correct? |
|--------|---------------|-----|----------|
| ImgBurn "Disc Information, Free Sectors" | **24,438,784** | **12,219,392** | yes, the real break |
| Windows IMAPI `TotalSectorsOnMedia` | 23,652,352 | 11,826,176 | no, a partial format-capacity descriptor, 0.8 GB too early |

Feeding the wrong (smaller) number puts the guard zone about 0.8 GB before the real layer
transition, so live video ends up on the defect-prone sectors, which is the exact thing the
guard exists to prevent. Use the disc's full formatted capacity (ImgBurn "Free Sectors", or
the `READ FORMAT CAPACITIES` formatted or maximum descriptor), then divide by the layer count.

An orchestrator should read this number from the drive and pass it to `--layer-break-lbn`;
tsMuxeR itself never touches the burner. The GUI has a small calculator that turns the
ImgBurn "Free Sectors" value into the break sectors for you.

## Recommended flow (multi-layer disc with menus and guard)

1. Read the target disc's full capacity (for example ImgBurn "Free Sectors"). Divide by the
   layer count to get the break sector(s). For a standard 50 GB disc that is `12,219,392`.
2. `tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=<breaks> <BDMV_folder> out.iso`
3. Burn `out.iso` with verify. The layer break is media-fixed, so the burner switches layers
   at the same sector and the guard zone lands exactly on it.

## BD-R XL (100 / 128 GB) player compatibility

Many Blu-ray players cannot read 100 or 128 GB BD-R XL discs at all, and there is no
guarantee a given player will. Keeping the image around 66 GB (the first two layers) and
finalizing the disc improves the odds on some players, but even 66 GB is not guaranteed to
play. The full 100 or 128 GB needs a recent player that explicitly supports high-capacity
recordable media. Verbatim media and a slow (2x) burn give the most reliable results. This is
a player limitation, not an ISO one, so test on your own device.
