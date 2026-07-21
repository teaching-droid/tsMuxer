# Dual-layer and BD-R XL authoring additions to tsMuxeR

This fork adds multi-layer (BD-R/RE DL and BD-R XL) authoring features on top of
jaminmc/tsMuxer. All of them are opt-in; the default behaviour is unchanged.

## New options (mux mode)

| Option | Purpose |
|--------|---------|
| `--disc-size=<bd25\|bd50\|bd100\|bd128 \| bytes>` | Fit-to-disc guard. Aborts before muxing if the estimated image will not fit the target disc. |
| `--allow-oversize` | With `--disc-size`, turn an over-capacity overrun from a hard error into a warning. |
| `--layer-break-guard=<MB>` | Zero-fill `<MB>` after each layer break (the start of the next layer, where discs are defect-prone), plus a proportional margin before it: one sixteenth of the after value (the 64:4 ratio of the original design), at least 4 MB. So 64 keeps the old 4 MB before, 288 gives 18 MB before. No file data lands on those sectors; the file that crosses the break (usually the movie) stays one logically contiguous UDF file, two extents around the gap, and plays seamlessly through read-ahead. The zone is asymmetric on purpose: real-hardware testing showed the next-layer defect can reach about 35 MB while the tail of the previous layer is clean, so the budget goes where the defect is. Use `288` (the GUI default: covers the reported 35 to 258 MB defect zones and the shifted break of defect-managed discs). `0` aligns without filler. Off when not given. |
| `--layer-break-guard-before=<MB>` | Optional. Size the zone BEFORE each break on its own, instead of the proportional default margin. The default is asymmetric because the measured defect sits at the start of the next layer, but some media are also weak just before the break, so this lets you pad both sides. Leave unset to keep the asymmetric default. |
| `--layer-break-lbn=<sector[,sector...]>` | Set the layer break sector(s). One value for BD-R/RE DL, two for 100 GB BD-R XL, three for 128 GB BD-R XL (see below). |
| `--disc-capacity=<sectors>` | Optional, `--bdmv-to-iso` only. The disc's total Free Sectors; lets the layer-fit placement check that moving a file wholly past a break still fits on the disc. Without it a conservative capacity is derived from the break spacing. The GUI passes this automatically. |
| `--original-order` | Optional, `--bdmv-to-iso` only. Write the files in their numeric (playback) order instead of largest first; for seamless-branching discs whose many segments should stay close to their playback order. |
| `--no-layer-fit` | Optional, `--bdmv-to-iso` only. Disable the layer-fit placement (see below) and always split whichever file crosses the guard zone. |

## New mode: `--bdmv-to-iso`

```
tsMuxeR --bdmv-to-iso [--layer-break-guard=<MB>] [--layer-break-lbn=<sector[,sector...]>] <BDMV_folder> <out.iso>
```

Wraps an existing, unprotected BDMV folder into a UDF 2.50 BD-ROM ISO byte-for-byte, with
no re-mux and no re-numbering. Every `.bdjo`, `.jar`, `.clpi`, `.mpls`, `index.bdmv`,
`MovieObject.bdmv` and the `CERTIFICATE/` chain is copied unchanged, so BD-J menus and all
clip and playlist references stay valid (the guard band only moves UDF extents, which the
filesystem hides from the player). The largest `.m2ts` is written first so the main title
straddles the layer break and receives the guard band (or use `--original-order`, see the
options above). MakeMKV helper folders are skipped.

Layer-fit placement (on by default): when the file that would cross a guard zone fits
completely between that zone's end and the next zone (and within the disc capacity), it is
placed whole on the far side of the break instead of being split. Two typical wins: a disc
with two big titles (theatrical and director's cut) gets one title per layer with the break
cleanly between them, and a seamless-branching disc's many segments arrange so the break
falls between segments, the same way commercially authored discs place it. A file larger
than a layer still straddles the break with the guard inside it, as before.

After the build, the log and a `<out.iso>.layerbreak.txt` sidecar report each guard's
position: the zeros' sector range, the file involved, the byte offset inside it, and for a
stream file the approximate playback time of the break. Note the seamless-branching
limitation described in the GUI section: inside a segment file the time is segment-relative.

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
| Windows IMAPI `TotalSectorsOnMedia` | 23,652,352 | 11,826,176 | no for a BD-R DL: 23,652,352 is the defect-managed (BD-RE DL) capacity, 0.8 GB too early |

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

## In the GUI (the "BDMV folder to ISO" tab)

The GUI exposes the same features with no command line. On the "BDMV folder -> ISO" tab:

- **BDMV folder and output ISO.** Point the folder at the disc root that holds `BDMV/` (and
  `CERTIFICATE/`). A mounted ISO drive works as the folder too.
- **Disc type with pre-filled Free Sectors.** Pick the disc from the list (BD-R DL 50 GB,
  BD-RE DL 50 GB, BD-R XL 100 GB, BD-R XL 128 GB) and its standard "Free Sectors" value is
  filled in and **locked** automatically, so you do not have to run ImgBurn and cannot change
  the value by accident; the layer break(s) are calculated for you. BD-R DL and BD-RE DL are
  separate entries because they differ: a blank BD-R DL holds 24,438,784 sectors (full
  capacity), while a BD-RE DL holds 23,652,352 (it reserves defect-management spare). If your
  disc is non-standard (a reformatted BD-RE, or a BDXL burned without defect management), tick
  **Enter Free Sectors manually (advanced)** to unlock the field and type the value ImgBurn
  shows for your exact disc.
- **Layer-break guard (after break).** The guard size in MB, with a colour-coded hint. The
  default is 288 MB: reported real defect zones cluster around 35, 64 and 258 MB, and on a
  defect-managed disc the true layer switch can sit up to 128 MB after the calculated break, so
  288 covers all of these. Rare media with defect zones over 1 GB need a larger value (the
  field accepts up to 9999). Tick **Also fill before the break (advanced)** to reveal a second
  field and pad the zone before the break as well, for media that are weak on both sides of the
  transition.
- **Keep original file order (seamless branching).** Files are normally written largest first,
  so the main movie sits on the layer break and receives the guard. Discs that use seamless
  branching store the movie as many segment files played in sequence; ticking this keeps the
  files in their numeric (playback) order so segments stay physically close to the order they
  play in, like on the original disc. In both modes, when the file that would cross the break
  fits entirely on the next layer it is placed there whole, so the break falls cleanly between
  files instead of splitting one; this is how commercially authored discs place their layer
  breaks too.
- **Layer break report.** After the build, the log shows where each guard landed: which stream
  file, the byte offset inside it, and (for a normal single-file movie) the playback time, so
  you know where to spot-check on a player. The same information is written next to the image
  as `<name>.iso.layerbreak.txt`. Current limitation: on a seamless-branching disc the time
  shown is the position within that segment file, not within the whole movie. A whole-movie
  time would require trusting the disc's playlists, and many discs ship decoy playlists, so
  the tool reports the segment and lets you locate it by chapter instead.
- **Fit estimate.** A live line shows the estimated image size against the disc's capacity as
  you fill in the folder, disc type, Free Sectors and guard: green when it fits (and how much
  space is left), red when it does not (and by how much). The guard band counts toward the
  estimate, so raising the buffer updates it at once.
- **Build ISO** runs the same `--bdmv-to-iso` command with those values.

## Seamless branching: what it is, and what it can and cannot affect

On the disc, a seamless-branching movie is not one file: it is many segment files
(`00001.m2ts`, `00002.m2ts`, ...). The playlists are small scripts that say "play segment 3,
then 7, then 12"; the theatrical cut takes one path, the directors cut another, sharing most
segments. The "seamless" part happens at playback time in the player's buffer. It is
choreography, not anything stored differently in the bytes.

**What branching affects: the placement, not the correctness.** The build normally writes
files largest first, so on a branching disc the segments end up ordered by size rather than
by playback order, which costs some seek distance on a slow drive; that is exactly what the
"Keep original file order" option is for. With layer-fit placement the break falls between
two segments either way, the same spot commercial authoring uses. And the playback time in
the layer break report is relative to the segment file it names, not to the whole movie (see
the report note above).

**What branching cannot affect: verification.** The guard check reads raw 2048-byte sectors
at the break positions and asks one question: are these bytes zero? Whether the surrounding
data belongs to one large movie file, to segment 47 of a branching disc, or to the gap
between two segments, a zero band at a disc position is a physical fact, independent of any
playback logic layered on top. The content check hashes every file on both sides; a branching
disc simply has more files in the list, and the playlist choreography that stitches them at
playback time does not change a single byte inside any file. If every segment hashes
identical, the content survived, whatever order any cut plays them in.

An analogy: think of shipping a box of books. The guard check confirms the protective padding
sits at the right spot in the box, whichever books surround it. The content check compares
every book, page by page, against the original shipment list. Seamless branching is the fact
that the reader will later jump between the books in a choose-your-own-adventure order; that
is a reading behaviour, and it cannot change what is in the box or whether the padding is in
place.

## Standard capacities the GUI pre-fills

The disc-type dropdown fills these blank-disc "Free Sectors" values (2048-byte sectors), the
same numbers ImgBurn reports for a normal blank disc, and locks the field so the value cannot
be changed by accident:

| Disc | Free Sectors | Note |
|------|-------------|------|
| BD-R DL 50 GB | 24,438,784 | write-once, full capacity |
| BD-RE DL 50 GB | 23,652,352 | rewritable, reserves defect-management spare |
| BD-R XL 100 GB | 47,305,728 | BDXL, defect-managed |
| BD-R XL 128 GB | 60,403,712 | BDXL, defect-managed |

These are the defect-managed (normal) capacities. The numbers are facts about the media: they
were read from real Verbatim discs (ImgBurn "Free Sectors") and match the Blu-ray/BDXL spec. A
disc burned WITHOUT defect-management formatting has a larger full capacity (for example
48,878,592 for 100 GB or 62,500,864 for 128 GB); if ImgBurn shows one of those for your disc,
tick **Enter Free Sectors manually (advanced)** and type it. BD-RE capacity in particular
depends on how the disc was formatted, which is why the field can be unlocked.

## Defect-managed discs (measured on real hardware)

A BD-R DL formatted WITH defect management (spare areas) behaves differently from a plain one,
and both effects were measured on a real burned disc, not taken from a datasheet:

- **Capacity.** The spare areas reduce Free Sectors by exactly their size. ImgBurn's Disc
  Definition Structure lists them in clusters of 32 sectors; the `TDP` value in ImgBurn's
  Format Capacities is the spare total (for example `TDP: 24576` clusters = 786,432 sectors =
  the exact difference between 24,438,784 and 23,652,352). A defect-managed BD-R DL therefore
  uses the BD-RE DL capacity entry.
- **The layer switch is NOT at capacity divided by layers.** The spare areas can be split
  unevenly between the layers (a real ImgBurn format produced ISA0 4,096 + OSA0 6,144 on layer
  0 but ISA1 8,192 + OSA1 6,144 on layer 1). Each layer then holds a different amount of user
  data, and the true switch sits at `layer size minus (ISA0+OSA0) x 32` sectors, which was
  measured 128 MB AFTER capacity/2 on a real disc (a read-timing scan shows the drive's
  refocus pause at the transition). BDXL 100/128 GB formats have symmetric spares per layer,
  so their even split IS exact.
- **What to do.** Nothing special: the default 288 MB guard covers the shifted switch with
  margin, and this was verified end to end (the disc plays seamlessly across the transition).
  Only when using a small custom guard on a defect-managed disc should you place the break
  manually with `--layer-break-lbn` using the formula above.
- **Trade-offs of defect management for video.** The drive verifies while writing and remaps
  bad clusters into the spare areas by itself, which roughly halves the burn speed. A remapped
  cluster is physically relocated, so reading it during playback forces a head seek; data
  discs do not care, but for video a plain (non-managed) burn with the guard band gives the
  smoother result. Defect management and the guard band solve different problems: remapping
  preserves the data, the guard keeps the picture running without seeks.

## Guard size on the burned disc (sector alignment)

The guard fills whole 2048-byte sectors, and it starts at the end of the movie's last data extent
rather than at an exact byte offset. The zero band around the layer break therefore snaps to sector
and file-extent boundaries: on a finished disc it matches the size you set closely (within about
1 MB) but not to the exact byte, and it tends to be equal or a little larger, never meaningfully
short. For example, setting 160 MB per side reads back as about 160.2 MB. That is normal alignment,
not a defect. Because the guard is sized in tens to hundreds of MB to cover the roughly 35 MB defect
region at the layer transition, a sub-MB alignment difference has no effect on playback. So if you
read a finished disc and the zero band is not exactly the value you entered, that is expected.

## BD-R XL (100 / 128 GB) player compatibility

Many Blu-ray players cannot read 100 or 128 GB BD-R XL discs at all, and there is no
guarantee a given player will. Keeping the image around 66 GB (the first two layers) and
finalizing the disc improves the odds on some players, but even 66 GB is not guaranteed to
play. The full 100 or 128 GB needs a recent player that explicitly supports high-capacity
recordable media. Verbatim media and a slow (2x) burn give the most reliable results. This is
a player limitation, not an ISO one, so test on your own device.
