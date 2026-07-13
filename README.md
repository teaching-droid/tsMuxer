# tsMuxer

> **Note:** This is a fork of [jaminmc/tsMuxer](https://github.com/jaminmc/tsMuxer) (itself a modernized C++20/Qt6 fork of [justdan96/tsMuxer](https://github.com/justdan96/tsMuxer)). It adds multi-layer disc authoring: a menu-preserving BDMV to ISO mode, a layer-break guard band for BD-R/RE DL and BD-R XL media, a fit-to-disc capacity guard, and a layer-break calculator in the GUI. See [DISC_AUTHORING.md](docs/DISC_AUTHORING.md) (also available [auf Deutsch](docs/DISC_AUTHORING_DE.md)).

## Vision

This project is for tsMuxer - a transport stream muxer for remuxing/muxing elementary streams. This is very useful for transcoding and this project is used in other products such as Universal Media Server.

EVO/VOB/MPG, MKV/MKA, MP4/MOV, TS, M2TS to TS, M2TS, or MKV.

Supported video codecs H.264/AVC, H.265/HEVC, H.266/VVC (Alpha release), AV1, VC-1, MPEG2. 
Supported audio codecs AAC, AC3 / E-AC3(DD+), DTS/ DTS-HD, TrueHD, Opus, FLAC, LPCM, MP3.

**TrueHD note (Blu-ray output):** Blu-ray players typically expect Dolby TrueHD to be muxed in a Blu-ray style
interleaved stream with an AC-3 compatibility core. If your MKV stores TrueHD and AC-3 as separate tracks, tsMuxer
can merge them at mux time via the meta/GUI option `merge-ac3-track` (see `docs/USAGE.md`).

Some of the major features include:

* Ability to set muxing fps manually and automatically
* Ability to change level for H.264 streams
* Ability to shift a sound tracks
* Ability to extract DTS core from DTS-HD
* Ability to join files
* Output/Author to compliant Blu-ray Disc or AVCHD
* Matroska (MKV/MKA) muxing support
* Blu-ray 3D support

Additions in this fork (see [DISC_AUTHORING.md](docs/DISC_AUTHORING.md) for details):

* `--bdmv-to-iso`: wrap an existing unprotected BDMV folder into a burnable BD-ROM ISO byte-for-byte, keeping BD-J menus and all clip/playlist references intact
* `--layer-break-guard`: zero-fill the defect-prone sectors around each layer transition of BD-R/RE DL and BD-R XL media, so the movie plays seamlessly across the break (validated on real hardware)
* `--layer-break-lbn`: set the layer break sector(s); takes a comma list for 100/128 GB BD-R XL (2 or 3 breaks)
* `--disc-size` / `--allow-oversize`: abort (or warn) before muxing if the image will not fit the target disc
* GUI: a "BDMV folder -> ISO" tab with a layer-break calculator (you copy the blank disc's "Free Sectors" value out of ImgBurn and paste it in; the break sectors are calculated for you), colour-coded guard hints, and input sanity warnings
* GUI: fully translated interface (German, Spanish, French, Hebrew, Japanese, Russian, Chinese)

## Ethics

This project operates under the W3C's
[Code of Ethics and Professional Conduct](https://www.w3.org/Consortium/cepc):

> W3C is a growing and global community where participants choose to work
> together, and in that process experience differences in language, location,
> nationality, and experience. In such a diverse environment, misunderstandings
> and disagreements happen, which in most cases can be resolved informally. In
> rare cases, however, behavior can intimidate, harass, or otherwise disrupt one
> or more people in the community, which W3C will not tolerate.
>
> A Code of Ethics and Professional Conduct is useful to define accepted and
> acceptable behaviors and to promote high standards of professional
> practice. It also provides a benchmark for self evaluation and acts as a
> vehicle for better identity of the organization.

We hope that our community group act according to these guidelines, and that
participants hold each other to these high standards. If you have any questions
or are worried that the code isn't being followed, please contact the owner of the repository.


## Language

tsMuxer is written in C++20. It can be compiled for Windows, Linux and Mac.

**Build Requirements:**
- CMake 3.12 or later
- C++20 compatible compiler (GCC 10+, Clang 11+, MSVC 2019+; the Windows release builds use Visual Studio 2022)
- Qt 6 for the GUI. The Windows release builds link Qt 6.8.3 statically, using the
  [qt6windows7](https://github.com/crystalidea/qt6windows7) patches, and build Qt from source with
  Ninja (the Visual Studio generator cannot build Qt). See [COMPILING.md](docs/COMPILING.md).

**Windows 7 Users:** the Windows release binaries are built against Qt 6.8 with the
[qt6windows7](https://github.com/crystalidea/qt6windows7) compatibility patches, so the
standard 32-bit and 64-bit builds run on Windows 7 and newer. No separate build is needed.

## Downloads

Ready-made binaries are produced by the build workflows: open the Actions tab, pick the
newest "Build for Windows 64-bit" (or 32-bit / Linux / Mac) run and download its artifact.
The Windows zip contains `tsMuxeR.exe` (command line) and `tsMuxerGUI.exe`, both
self-contained. Tagged versions additionally appear on the Releases page.

## History

This project was created by Roman Vasilenko, with the last public release 20th January 2014. It was open sourced on 23rd July 2019, to aid the future development.

## Installation

Please see [INSTALLATION.md](docs/INSTALLATION.md) for installation instructions.

## Usage

Please see [USAGE.md](docs/USAGE.md) for usage instructions.

## Todo

The following is a list of changes that will need to be made to the original source code and project in general:

* the program doesn't support MPEG-4 ASP, even though MPEG-4 ASP is defined in the TS specification
* has issues with 24-bit DTS Express
* issues with the 3D plane lists when there are mismatches between the MPLS and M2TS
* AV1 in MPEG-TS playback depends on player/demuxer support for the AOM draft specification (not yet widely supported as of 2026)

## Contributing

We’re really happy to accept contributions from the community, that’s the main reason why we open-sourced it! There are many ways to contribute, even if you’re not a technical person.

We’re using the infamous [simplified Github workflow](http://scottchacon.com/2011/08/31/github-flow.html) to accept modifications (even internally), basically you’ll have to:

* create an issue related to the problem you want to fix (good for traceability and cross-reference)
* fork the repository
* create a branch (optionally with the reference to the issue in the name)
* make your changes
* commit incrementally with readable and detailed commit messages
* submit a pull-request against the master branch of this repository

We’ll take care of tagging your issue with the appropriated labels and answer within a week (hopefully less!) to the problem you encounter.

If you’re not familiar with open-source workflows or our set of technologies, do not hesitate to ask for help! We can mentor you or propose good first bugs (as labeled in our issues). Also welcome to add your name to Credits section of this document.

All pull requests must pass code style checks which are executed with `clang-format` version 18. Therefore, it is advised to install an appropriate commit hook (for example [this one](https://github.com/barisione/clang-format-hooks)) to your local repository in order to commit properly formatted code right away.

## Submitting Bugs

You can report issues directly on Github, that would be a really useful contribution given that we lack some user testing on the project. Please document as much as possible the steps to reproduce your problem (even better with screenshots).

## Building

For full details on building tsMuxer for your platform please see the document on [COMPILING](docs/COMPILING.md).

## Testing

The very rough and incomplete testing document is available at [TESTING.md](docs/TESTING.md).

## Financing

We are not currently accepting any kind of donations and we do not have a bounty program.

## Versioning

Version numbering follows the [Semantic versioning](http://semver.org/) approach.

## License

We’re using the Apache 2.0 license for simplicity and flexibility. You are free to use it in your own project.

## Credits

**Original Author**
Roman Vasilenko (physic)

**Contributors**
* Daniel Bryant (justdan96)
* Daniel Kozar (xavery)
* Jean Christophe De Ryck (jcdr428)
* Stephen Hutchinson (qyot27)
* Koka Abakum (abakum)
* Alexey Shidlovsky (alexls74)
* Lonely Crane (lonecrane)
* Markus Feist (markusfeist)
* jaminmc
* teaching-droid

<sub><sup>For sake of brevity I am including anyone who has merged a pull request!</sup></sub>
