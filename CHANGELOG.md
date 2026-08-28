## tsMuxeR 2.18.0

- A CORRECTION TO 2.17.0 FIRST. That release claimed, without qualification, that a dual layer Dolby Vision disc taken into one Matroska track and split back out produces a Blu-ray byte identical to authoring it straight from those layers. It was measured on one disc and stated for all of them. On a disc that frames its start codes the shorter way it was false: the rebuilt base layer first differs 736 bytes in. What differed was the framing alone. Every picture and every piece of Dolby Vision metadata came back exactly even on that disc, both forms are conformant and decode identically, and a disc authored with 2.17.0 plays as it should, so this costs nobody a rebuild. The 2.17.0 entry has been corrected in place and the README line with it. It is being said here as well because the release notes are mirrored where they cannot be edited, and because a claim of this kind is the sort of thing people choose an archive format on. The claim now holds as it was written: the entry below has the figures, on a whole feature rather than a clip.
- A cover art tag in front of an AAC stream was demuxed as audio. An AAC sync is twelve bits and two more with no checksum, so arbitrary data satisfies it about once every sixteen thousand bytes, and in front of the audio that arbitrary data is usually a picture: a file carrying 1.2 MB of cover art produced 455,104 bytes where the audio is 194,146, beginning inside the picture with a header a decoder reads as 32000 Hz and four channels where the truth is 48000 Hz mono. The same file could not be opened at all, since the search is retried only five times and escapes about ten bytes of junk, so it was refused with "Can't detect stream type" instead of muxed. Requiring a second header exactly one frame length on removes those false syncs, since two matches at a distance the first one computed are far beyond coincidence, but applied everywhere that also destroys real audio, and not by accident: a genuine frame whose successor is damaged is indistinguishable from a false sync, so refusing it throws away every real frame in front of the next confirmable one. One flipped byte cost 881 bytes of undamaged audio, and a file whose frames are separated by junk lost 193,449 of its 194,146 at exit 0 and in silence. The two cases cannot be told apart by looking at the sync, nor by counting the frames after it; they are identical on every such measure at every depth. What separates them is where the reader is standing, because refusing a sync cannot cost audio where there is no audio, and a metadata tag is exactly that place. So a sync may now be disbelieved inside a tag the reader has walked and verified and nowhere else, verified rather than merely declared, since a length taken on trust would let a few bytes of forged header arm a skip over real audio. ID3v2 counts only when the whole tag is present and its body walks, APEv2 uses the conservative half of the walk added for the loss report, the two fixed size tags are their own proof, and Lyrics3 does not feed it at all because it is found by searching for its closing keyword rather than by stepping through declared sizes. Detection was given the same ceiling, because that is where the unreadable file was. With no verified tag the ceiling is the search position itself and the first candidate is returned, which is what the reader already did: across a corpus of twenty one files the demuxed track is byte identical to the previous release on twenty of them, and the twenty first is the cover art file, where it is byte identical to the audio stripped out by hand. A tag that does not verify buys nothing on purpose, so the same file with its length field corrupted, and a trailing tag whose body is a repeated false sync, both leak exactly as they did before. That is the trade, and it is the right way round: a leak at the end of a file is visible and reaches nothing, while the leak this removes is at the front, where it mis-declares the stream and stops the file being opened.
- A track that lost data on the way through now says so, with the figure, instead of finishing at exit 0 in silence. A damaged source, a joined part that does not end on a frame boundary, or a codec name that is not the one the track holds each cost audio quietly, and the only way to notice was to compare sizes by hand. Most of the work is in what it refuses to report, because a warning on a file that is perfectly fine is worse than no warning at all: a metadata tag is not lost audio, and counting one as lost put a warning on ordinary tagged music, where two tagged MP3s joined announced 4,239 bytes dropped on output byte identical to the same join without the tags. So the tags that sit in front of, behind and between elementary streams are recognised by their own declared lengths, and every one of them is validated rather than believed. An ID3v2 body is walked frame by frame and its padding must be zero, which the specification requires in those words, so ten bytes of damage that happen to read "ID3" no longer swallow the rest of the file. An APEv2 item list is walked and must land exactly on the declared size; without that a well formed 32 byte header over a dropped region hid the loss entirely, one file reporting "1000032 bytes of the 2000626 read" and the same bytes behind such a header reporting nothing. Lyrics3 v2 declares a size per field so the fields are stepped through, replacing a bounded search that charged every tag past its bound as lost audio, where a legal 900 KB tag produced "900099 bytes ... dropped" on a file whose output is identical to the same file carrying no tag at all. ID3v1 needs more than the three bytes "TAG" to justify stepping over 128, since a legal MLP frame header is those same three bytes and cost 18,926, so the four year characters must be digits, spaces or nul. A TrueHD track with an AC-3 core merged into it counts both halves, which it did not: the report claimed fewer bytes read than the track was written out at, and a loss in the AC-3 half had no path to the figure, the same damage reporting "202496 bytes of the 1120000 read" as an ordinary track and nothing at all once merged. Each merged figure is now exactly the two halves added up, checked on five damaged files by reading every companion again on its own. The limits are worth stating rather than glossing. The tag walks validate structure, and an item value is opaque by design because it carries cover art, so a deliberately crafted tag can declare one item whose value is the audio and be believed, at a cost of about eleven bytes; that defends against corruption, which is what occurs in practice, and not against forgery, which nothing confined to this reader can do. Data lost at the very start or the very end of a track is still not reported, so a track that is mostly junk at the end can report very little, and a companion file longer than the track it is merged into is counted in the total although the excess could never have been used. Nothing here changes a single output byte: the figure is a report, no decision is taken from it, and every demuxed track is byte identical to the same job before the change.
- An AAC track could lose one frame, and always the same one, when the audio began close to the end of a read block. The reader answers "not a frame" and "there is not enough data yet to say" with the same value, so a header cut short by the block end was treated as junk: the position advanced by one byte, the byte was charged as lost, and the frame was gone. The window is exactly one header wide, which is what made it hard to see. Measured on a file whose audio begins six bytes before the end of the first block, with the data in front of it carrying no false sync of its own, a junk prefix one byte shorter comes out at 194,146 bytes and the whole stream, and the prefix that crosses the boundary comes out at 193,637, short by 509, which is exactly the first frame's declared length, at exit 0 and with no warning. AC-3 has always answered this properly and AAC now does the same, which is a three line change. The same branch is also why such a file could take minutes rather than seconds, since the reader ground through the whole window one byte at a time; a set of four test files that took over two minutes now takes seventeen seconds. That is only the case where a header is cut short by a block end, and the broader cost of rescanning a region already searched is a separate matter, untouched here.
- The start code framing of the source is now carried through Matroska, which is what makes the sentence above true. A start code is three bytes or four, both legal and identical once decoded, and discs differ: one measured here uses four bytes for the access unit delimiter and the parameter sets and three for everything else, which is the conformant minimal form, while two others use four throughout. Matroska stores these codecs length prefixed, so no start code exists in the file to copy, and a disc rebuilt from one always came out four byte. Authoring the same disc directly, with no Matroska in between, already preserved the framing, which is what made this visible as a property of one code path rather than of the disc. The framing is now observed as the source passes, recorded per NAL type rather than as one value for the file, because that is what the discs actually do and a blanket three byte mode would not be conformant, and reproduced on the way back out. Nothing is recorded that cannot be reproduced: a NAL type seen framed both ways records nothing, a source that used four bytes throughout records nothing since that is already the default, and a rule that cannot be read back changes nothing, so an ordinary file gains nothing and the worst case is no improvement rather than wrong output. The disc that had never come back identical now does, over the whole 73 GB feature and through both Dolby Vision profiles. It applies to every way out of a Matroska file and not only to a Dolby Vision rebuild, so this is not a Dolby Vision matter at all: an ordinary single layer disc taken to Matroska and authored back reproduces its source framing as well, where before it came out four byte whatever the source did. Measured against a build of the previous version so the comparison is of this change and nothing else: a base layer that differed at byte 736 is identical for all 294,346,414 bytes of the rebuilt stream, and an H.264 stream framed three byte throughout comes back with 6,834 three byte codes and no four byte ones, the same as it went in.
- --dv-profile=8.1 is available again, held back in 2.17.0 because the conversion could not be undone. A dual layer disc can now be stored as single layer profile 8.1, which many more devices accept, and still be turned back into the disc it came from. The conversion is many to one, so no arithmetic recovers a profile 7 RPU from an 8.1 one; instead the disc's own RPUs travel with the file as an attachment, in the layout an extracted RPU file uses so anything else can read it, and are put back when the file is split. They are attached in presentation order rather than the decode order the mux collects them in, because that is the order every other tool assumes: measured on four discs the two orders part company for 83 to 93 percent of pictures while running no more than seven apart, so decode order would have produced a file that parses happily everywhere and is used wrongly. A third attachment carries the presentation time of every entry so a picture is looked up rather than counted to. Everything the file promises is checked before a single picture is written, both attachments present, both matching their recorded checksums, one timestamp per entry and the count and order as claimed, and a file that fails any of it is refused, because the alternative is a disc that looks right and is wrong. Asking for it where there is nothing to convert is now refused with the reason, rather than accepted in silence: a single layer file already carries its picture and its RPU in one track, so the option used to do nothing at all and the file came out exactly as it would have without it, with nothing in the log to say so. Measured on whole titles rather than clips, since a clip exercises neither a tail nor a join and both mattered: a 73 GB feature comes back byte for byte identical on both layers, 59.33 GB of base layer and 4.25 GB of enhancement layer with nothing left over on either side, with all 166,928 of the disc's original metadata records restored, and that holds through profile 7 and profile 8.1 alike, so the disc is reproduced exactly while its metadata is converted and put back; and a seamless branching title of 23 clips and 22 joins, 2:22:52, where the profile 7 and profile 8.1 rebuilds are byte identical on both layers.
- The seek index is now also written at the front of the file. Cues have always gone after the clusters, because their contents are not known until the mux has finished, and nothing at the head of the file pointed at them. That was survivable for an index and is not survivable for an attachment: checked against other software, the attachments were not listed at all, and copying such a file produced one with the video intact and the attachments silently gone, which for a profile 8.1 carrier means one pass through another muxer destroys the only copy of what the disc is rebuilt from. 256 bytes are reserved at the front of the segment and the real index is written over them at close. The index at the end stays, so nothing that relied on it changes, and if the reserved space ever failed to hold the entries the file is still correct and the log says the index is only at the end.
- A merged Dolby Vision track now lists as its two layers, so it can be separated in the interface instead of by hand writing a meta file with subTrack=. The interface needed no change for this: it has always read a sub track number out of the listing and written it back, and that code does not care which codec it came from. The numbering is the opposite way round from the MVC case that mechanism was built for, which calls the dependent view 1 where Dolby Vision calls the base layer 1, so the number is now carried on the listing entry rather than derived from the codec id. Deriving it worked for MVC only because its two halves have different codec ids; on two HEVC entries it would have swapped the layers and authored a disc with the quarter resolution enhancement layer as the picture, which nothing downstream would have caught.
- GUI: a Dolby Vision selector beside the output format, offering the disc as it is or profile 8.1. It is hidden rather than greyed out unless it applies, meaning Matroska output of a source that really carries a dual layer track, since a disabled control still invites the question of what it is for. Translated into all eight interface languages. There is also a new docs/DOLBY_VISION.md, in English, German and Japanese, covering the two layers and why Matroska needs them in one track, splitting a merged track apart again, both profiles, the library each platform needs, and what is preserved. It states the thing people most often expect and do not get: profile 8.1 does not make the file smaller here, because the enhancement layer and the disc's own metadata are kept so that the disc can be rebuilt, and that is the point of it.
- An E-AC-3 track carrying Atmos now says which kind it is and how many objects it holds, "+ ATMOS (JOC, 16 objects)". It used to print the same three words as a TrueHD track carrying Atmos, though the two are different things, joint object coding in the addbsi field against a fourth substream. The object count was already being decoded and discarded, and it is the complexity index of the Type A extension, so a sixteen object mix looked exactly like a minimal one. The more important half is that the test was hardened: reaching that field means walking a dozen variable length fields, so one frame landing on the pattern by accident used to latch the badge on for a whole track and it was never cleared. Frames must now agree on the same non zero object count before the track is named, the same discipline the DTS:X badge uses, and a frame that disagrees restarts the count rather than adding to a majority built out of noise. TrueHD is deliberately unchanged: its Atmos is inferred from the substream count rather than read from a payload, which deserves its own evidence rather than being altered while something else is being changed.
- Fixed the H.264 reader assuming a four byte start code. It read the NAL type from a fixed offset, which on a three byte framed stream is one byte into the header, so the type came out wrong, an access unit delimiter already present was not recognised, and the muxer added a second one. The HEVC and VVC readers have always worked the length out from the byte in front of the start code, which is exact, and this one now does the same, so four byte material behaves exactly as before. Verified on a real stream reframed to three bytes: 841 delimiters in and 841 out, one per picture and no duplicates, with everything else byte identical.

- For anyone who builds from source: the version line named the commit the build directory was CONFIGURED with, not the one that was built. The commit is read when cmake configures and baked into the compile flags, so rebuilding in an existing directory never refreshed it, and a build here reported a commit 24 behind the code it contained. Releases were never affected because the build service configures a fresh tree every time; it is a local build that misreports itself, which is exactly the position someone is in when they build from source to report a problem. cmake now re-runs when HEAD moves, watching the branch reference as well since a commit on the current branch does not rewrite HEAD, and the packed form for a tree whose references are packed. Where git is absent nothing is watched and a source tarball builds as before.
- Writing a BDMV folder to an ISO could SILENTLY LOSE THE LAST BYTES OF A FILE. A file that does not end on a sector boundary keeps its final partial sector in a buffer and writes it separately, and the position for that write was measured from the start of the FILE but added to the start of the LAST EXTENT. Those are the same place only while a file has one extent, which is why this survived: a file grows a second extent only past a gigabyte or where a layer break guard splits it, and only an unaligned length reaches the branch at all. The loud half of the fault was an image at roughly twice its proper size, with the data at the front, a hole of nearly the whole payload, and the closing structures at the end. The half that matters is quieter: those last bytes went into the padding past the end of the image and the sector they belonged in kept its zeros, so the image mounts, every file inside reads at its correct LENGTH, and the tail is gone. A genuine stream file is written in units of three sectors so it can never trigger this; extra files carried onto the image with --keep-extra-files can. Measured with a source whose every byte is checkable rather than a zero filled one, because a zero filled source cannot tell correct data from a hole: before the fix a 2.3 GB file read back off the mounted image differs from its source and its last 4096 bytes differ, after it the file is identical byte for byte. The arithmetic matches the observed inflation exactly, 3,170,893,824 bytes predicted and measured on a 3 GB reproducer. ANYONE WHO BUILT AN ISO THIS WAY WITH EXTRA FILES ON AN EARLIER RELEASE SHOULD CHECK IT.
- A muxing rate at or below 90 kbps made the muxer WRITE UNTIL THE DISK FILLED. The pacing expression adds a count of bits to a count of 90 kHz ticks, so it only stays under the media time while the rate in bits per second is above the tick rate, and below that the clock runs away from the media and the null padding follows it. The boundary is therefore the tick rate itself rather than a number anyone chose, and it lands on the kilobit: 91 kbps and everything above it is unaffected, 90 kbps produces two and a half times the correct size, 89 kbps turned a two second, 258,500 byte mux into 23.9 GB, and 35 kbps into 292 MB. Each of those reported "Mux successful complete" and exited zero. A 3.36 MB AC-3 file asked for 64 kbps produced 796 MB. The range is refused now, on both --bitrate and --maxbitrate, with a message that names the option, the value and the unit, and nothing is written before the refusal. --minbitrate is deliberately not checked, since on its own it never reaches that expression and zero is what the interface writes when no lower bound is wanted. The expression itself is left alone: correcting it would move the output of every mux that sets a bitrate, which is a separate piece of work with its own evidence, so this release fences off the range where it visibly detonates rather than pretending to cure it.
- The bitrate options did not do what the help said, in three separate ways. --maxbitrate was described as "The upper limit of the VBR bitrate" and is not a limit of any kind: a 1 Mbps ceiling on a 30.77 Mbps track produces a byte for byte identical file, and so does 40 Mbps, and so does leaving the option out. Nothing anywhere drops, delays or refuses payload because of a rate, and a muxer cannot turn a 30 Mbps stream into a 1 Mbps one without re-encoding it, so it is now described as what it is, a rate the transport stream is paced to, with the point spelled out that a value below what the streams need does not make the output any smaller. --bitrate was documented in Mbps with --bitrate=35 as its example; the parser multiplies by 1000, so the unit is kbps and the example was out by a factor of a thousand and sat inside the range that used to fill the disk. And --cbr and --vbr are read by nothing at all: both fell out of the bottom of the option loop in silence, so a meta reading "--cbr" and nothing else produced a variable bitrate mux identical to asking for nothing, with no indication that the request had gone nowhere. Both words are recognised now and three combinations that cannot mean what they appear to say so: --cbr with no --bitrate, --cbr and --vbr together, which the help itself forbids and nothing checked, and --vbr beside --bitrate. The mux is untouched by all of it, every case measured comes out at exactly the byte count it had before, and everything the interface writes stays silent. docs/USAGE.md carried the same three sentences and is corrected with it.
- Writing a BDMV folder to an ISO now checks the result against the target disc, which it never did. The muxing path has always refused an over-capacity build unless --allow-oversize downgrades it; the folder to image path had no check and no warning, so a user could be handed an image that silently would not burn. It refuses before writing anything when the payload plus the layer break guard zones already exceed the disc, warns inside the band where the answer is uncertain, and measures the finished image exactly before the run ends. --allow-oversize, which this path did not previously parse at all, downgrades both. Only certain quantities may refuse: payload plus guard zones is a true lower bound on the image, while the UDF structures and the guard's own overrun are bounded rather than known, so they may only warn. A flat reserve would have falsely refused a real build that fitted by 196,608 bytes. Making that check also found --inner-only overshooting the disc silently by 1.44 MB, because its margin had to cover both the UDF structures and a pad overrun of up to one copy buffer and could not; it now reserves the overrun as well and lands 22.56 MB under on the same build, confirmed on a real dual layer scenario where it went from 13.06 MB under to 21.06 MB under, predicted before the run. An image is never deleted on a refusal that comes after the write.
- A file placed at the ROOT of an ISO built with --keep-extra-files was stamped with the UDF metadata bit and would not open. The flag was read from the parent directory's object id, and both the root directory and the system stream directory are built with id zero, so anything at the root inherited a bit that says the file is metadata rather than content. Files inside a subdirectory were unaffected, which is why this went unnoticed. Reported by @DreckSoft.
- --split-size was 32 bit, so asking for 4 GiB wrapped to zero and turned splitting OFF entirely without a word, and 4.5 GB produced 205 MB parts. Two defects rather than one: the running comparison overflowed as well, multiplying a 32 bit packet count by the frame size, so widening the size alone would not have worked. Proven by forming a single 4,405,683,744 byte part from 4.7 GB of joined input, which the old arithmetic could not express. A size of zero or less is refused now rather than silently disabling splitting, since accepting it turns the feature off without a word, which is exactly what asking for 4 GiB used to do. Anyone whose meta asked for 4 GiB or more got one file and will now get real splitting.
- Splitting was ignored for Matroska output in silence. The same meta produced nine parts as a transport stream and one 421 MB file as Matroska, with a success message and exit code zero in both cases. Matroska has no split support here, and its option parser only ever looked for the Dolby Vision profile, so both split options fell past it unread. The engine says so now, mirroring the demux path which already did, and the interface greys the split controls for Matroska and demux output and stops writing the option into the meta at all, because greying alone would still have emitted it. The tick is preserved across a change of output format rather than cleared, so nothing a user set is lost by looking at another format.
- The Blu-ray settings tab took no notice of the chosen output format. Surveyed before anything was changed: 23 controls across all seven output formats, and not one of them changed state, while the meta builder had been dropping every one of them for the formats that cannot use them. So nothing was ever muxed wrongly; the tab was simply saying something untrue about itself. Chapters, the blank playlist, playlist and stream numbering and the 3D right eye flag now follow disc output, Force BD-ROM V3 follows Blu-ray and Blu-ray image output only since AVCHD has no V3, the default track flags follow those three and Matroska, and the mux start time stays live because it applies everywhere. Whole group boxes are disabled where one condition covers everything inside, which leaves each control's own rule intact for when the group returns. Nothing is cleared: a chapter length and a playlist offset set for disc output survive a trip through transport stream output and come back, and the meta produced for a disc is identical before and after that trip, which is the point of the change. The picture in picture group is deliberately untouched, since its condition is the selected track being secondary rather than the output format.
- The Merge AC-3 file box took what was typed into it and threw it away. Setting the track number beside it cleared the file box but left it enabled, so it still accepted keystrokes and discarded them on the next update, and clearing the track number did not bring the path back. The exclusion between the two is a real engine rule, verified by running all three forms, so no mux was ever wrong, but the interface was silently eating input. The control that does not apply is greyed now rather than wiped, both directions resolve so neither can deadlock, and a path typed into it survives.
- The Max bitrate box defaulted to 99.99 kbps, which is a genuine restriction of 99,990 bits per second, and neither bitrate field is remembered between sessions, so every session started there. On a 29 Mbps track that broke the Blu-ray requirement for a clock reference at least every 100 ms once in thirty seconds and reversed the sign of the drift against the presentation times. It is 48000 now, which is the rate this program already writes into the clip information for a Blu-ray, and at that value the output is byte for byte identical to not ticking the box at all, so the change cannot regress a mux that was working.
- The last picture of a stream that ends without an end of stream marker was lost on HEVC and duplicated on H.264, one coupled cause in code six stream readers share. Ground truth counted from the elementary streams rather than from the muxer's own report, and verified on a real interleaved 3D file as well as on plain streams.
- The last frame of the AC-3 core was dropped by every merge since the feature existed. The coverage warning's tolerance is re-derived to one AC-3 frame with it, so a file that is now complete is not reported as short.
- Fixed a crash and a silent half core, both around the AC-3 core merge. A merged track that was read back could produce a partial core with no warning, which is worse than the crash beside it because the file looks finished.
- A Matroska file written from a source that frames its start codes the shorter way carries a small text attachment recording how that source was framed, so a disc built from it can be framed the same way. That attachment was named as a Dolby Vision file, and the name reached files that have nothing to do with Dolby Vision: muxing the base layer of a pressed disc to an ordinary Matroska file, with no Dolby Vision option anywhere and no enhancement layer in the output, produced a file listing dv-manifest.txt in its attachments. Anyone opening it in a tool that shows attachments was told the file carries Dolby Vision data, which it does not. The file's own first line already reads "tsMuxeR stream notes" and only the name said otherwise, so it is tsmuxer-manifest.txt now. The name is a private agreement between the muxer that writes it and the demuxer that reads it back, so it is changed here rather than after a release carries it.
- GUI: the input file panel explains itself, both remove buttons say which is which, and the accents are back in the translations that had lost them. The Dolby Vision row no longer appears on a freshly opened window before a file has been loaded.
- THE DOLBY VISION PROFILE SELECTOR DID NOTHING. Every other combo box in that window is connected to something that refreshes the meta, and this one was connected to nothing at all. The meta file handed to the muxer IS the text in that preview, so a choice that never refreshed it never reached the mux either: picking profile 8.1 moved the control and changed nothing else, and the file came out as profile 7 with nothing in the log to say so. That held for every source, including the already merged Matroska file the feature was verified against when it was built. Two more faults in the same control came with it. It never appeared for a DISC, only for an already merged file, which is the opposite of what the documentation describes: a disc lists its two layers as independent video tracks while the rule wanted a sub track number that only a merged track has, so a user following the documentation loaded the disc, saw no selector and silently got profile 7. The rule now also recognises the disc shape, mirroring the muxer's own pairing, which is a video track after the first one that carries Dolby Vision data, and not merely any track with an RPU, because a single layer profile 8 track has one too and must be left alone. And it did not refresh when a track was removed or unticked, so removing the enhancement layer left it showing and put the option into a mux with nothing left to fold; the two missing refresh points are added and unchecked rows are skipped when it decides, since an unticked row reaches the muxer commented out. Measured in the running window afterwards on both source shapes: choosing profile 8.1 puts the option into the meta for a dual layer disc and for a merged file alike, where before neither did, while a transport stream output and a single layer source both correctly show nothing.
- A REFUSED DOLBY VISION MUX DESTROYED THE FILE IT REFUSED TO WRITE. Asking for profile 8.1 where there is no dual layer track to convert is refused with an explanation, but the refusal happened on the first packet, by which time the destination had been created and truncated: a 96,002 byte file came back as a 308 byte stub. The check now runs before the destination is opened, so it covers every route to the option and not only the interface one. It cannot be the exact check there, because that one needs a value built from the codec reader and a reader has parsed nothing that early; what is knowable is the track list, and a fold needs two HEVC video tracks, so fewer than two refuses and two or more lets the exact check decide as before.
- The two bitrate boxes could ask for a rate the muxer refuses. Neither declared a minimum, so the default of zero applied and a value inside the refused range could be typed in and written into the meta. Both now start at 91, the first rate the pacing can carry. The defaults are unchanged and far above it, so an interface nobody has touched produces exactly what it did.

## tsMuxeR 2.17.0

- The layer-break guard beside the normal Blu-ray output was placed at the BD-R DL break whatever disc was selected. The disc list there was a bare BD25/BD50/BD100/BD128 with no capacities behind it, and the break sectors were never passed to the muxer, so it fell back to half a 50 GB disc: on a 100 or 128 GB disc that sector is not a layer boundary at all, so the guard was placed at the wrong sector. Both parts of the interface now share one disc table, the one the BDMV to ISO tab uses, so the break is computed from the disc's real Free Sectors and the group shows where the guard will land before the mux runs.
- New "Also fill before the break" beside the normal Blu-ray output, matching the option the BDMV to ISO tab already had, for media that are weak just before the layer transition as well as after it. The muxer now accepts --layer-break-guard-before on an ordinary mux and not only on --bdmv-to-iso.
- The AC-3 core track of a TrueHD track is now written directly after the track it came from, rather than after every other stream in the file. That is where a disc remux normally carries it and where other muxers put it, so a file made here has the same track order as one made from the same disc elsewhere.
- The BDMV to ISO disc list now offers a 100 GB disc used across its first two layers only, and shows every disc's Free Sectors in its own entry. There is no 66 GB BD-R and many players cope with two layers better than three, so two thirds of a 100 GB disc is a common compromise; picking it puts the one break it needs on the disc's real first layer boundary and leaves the third layer unwritten, and the fit estimate, the layer-fit placement and --inner-only all treat the disc as 66 GB. It is still BD-R XL media, so the player-compatibility warning now follows the media rather than the layer count and still appears. Requested by @DreckSoft.
- The pre-filled Free Sectors for BD-R XL was the defect-managed capacity, which is not what a plain BD-R XL reports. The same disc size exists at two capacities, a defect-managed disc holding 1 GiB per layer back as spare area, and on a 100 GB disc that is 524,288 sectors: the calculated break lands a full GiB away from the real one, far outside any guard band, so the guard misses the break entirely. Both capacities have been measured here on real discs, so the entries now carry the full capacity, every entry shows its number so it can be matched against ImgBurn at a glance, and the difference and the defect-managed figures are spelled out where the value is chosen. A disc whose figure differs is still entered by hand, as before. Reported by @DreckSoft.

- A dual layer Dolby Vision disc is now carried in ONE Matroska track. Such a disc puts the picture on two video streams, a base layer plus a quarter resolution enhancement layer holding the Dolby Vision data, and Matroska needs both in a single track; until now a disc source produced two tracks and no player engaged Dolby Vision. Every enhancement layer NAL is wrapped in an unspecified NAL of type 63, which a decoder that knows nothing about Dolby Vision skips, and the RPU travels unchanged. The track carries the Dolby Vision configuration record describing profile 7 with base layer, enhancement layer and RPU all present, and beside it the enhancement layer's own HEVC configuration record. Nothing is re-encoded, re-ordered or re-escaped. Verified on a two hour feature, 166,928 frames all merged, against a file an independent muxer produced from the same disc: 166,927 of 166,928 blocks are byte identical, and the one that differs is the last, where the disc carries an end of stream marker on both layers that we keep and the other drops.
- The reverse direction as well: a merged track can be split back into its two layers for a disc, through the existing subTrack= mechanism, with the base layer on one PID and the enhancement layer on the other. A single layer file has no enhancement layer to separate and is refused with an explanation rather than producing an empty second track. CORRECTED AFTER RELEASE, see the 2.18.0 notes: this entry also said that two disc layers merged into one Matroska track and split again produce a Blu-ray byte identical to authoring it straight from those layers. That was not measured on every disc and it was not true on all of them. It held where a disc frames its start codes the way this muxer writes them, and not on a disc that uses the shorter conformant form, where the rebuilt stream first differs a few hundred bytes in. Every picture and every piece of Dolby Vision metadata did come back exactly even there, and the difference is legal either way and decodes identically, so no disc authored with 2.17.0 is defective. 2.18.0 carries the source framing through and the unqualified claim holds from that release.
- Fixed Matroska output keeping only the AC-3 core of a TrueHD track and throwing the lossless stream away. The result was a 448 kbps track labelled AC-3, with no warning and a successful mux, while the same source muxed to m2ts was intact. TrueHD detection only ever runs while probing, and the instance that does the muxing re-ran that probe on the Blu-ray path alone, so on every other output the reader believed it was plain AC-3 from beginning to end. The lossless stream is now carried and the track is declared A_TRUEHD, with the AC-3 core kept beside it on a track of its own (see below). Anyone who wants only the core can still ask for it with down-to-ac3. The other audio codecs were measured for the same fault rather than assumed safe: DTS-HD MA, DTS, AC-3 and E-AC3 all carry their full payload.
- The AC-3 core of a TrueHD track is no longer lost on the way into Matroska. A Blu-ray carries the lossless stream and a 448 kbps AC-3 core braided onto one PID, so a player that cannot decode the lossless part still has something to play; Matroska has no such arrangement, because a TrueHD track there holds the lossless stream alone. The core is therefore written as an ordinary AC-3 track beside it, which is the layout a disc remux normally has, and merge-ac3-track= braids the two back together when a disc is authored from that file. Without it the disc had no core, which the spec does not allow, and the audio could not be recovered from anywhere. Verified as a round trip from a pressed disc: the file comes back with the lossless track and a 448 kbps 5.1 track, the disc built from it reports "AC3 core + TRUE-HD + ATMOS" again, and the warning that MLP is not standard for BD disks is gone. The added track measures 449.4 kbps against the core's 448, the difference being one block header per frame, so it carries the core and nothing besides. Add drop-ac3-core to a track to leave it out.
- A DTS:X track using the IMAX variant of the marker is now named "+ DTS:X IMAX" instead of being shown identically to a plain DTS:X track. Measured over every frame of two tracks from pressed discs, 690,198 and 661,703 frames, all carrying the variant and none the plain marker. It names the audio encoding and nothing else: it does not say the title is an IMAX Enhanced presentation, which is a remaster and an expanded aspect ratio rather than anything in the bitstream.
- The level of an H.265/HEVC stream can now be changed without re-encoding, through the same level= track option the H.264 path has always had. general_level_idc is a single byte carried in both the VPS and the SPS, so raising it touches no picture data at all; a higher level is a superset of a lower one, so a stream that was conformant stays conformant. Until now the only way to alter that one number was to re-encode the entire track, which on a 4K feature is a full pass over the largest asset on the disc and a quality loss for a value in a header. Two things make it less trivial than it sounds and both are handled: the reserved bits immediately before the level are zeros, so emulation prevention bytes land in that exact region and the position has to be found by parsing rather than by a fixed offset; and the byte is written directly rather than through the general bit writer, whose safety margin refuses to reach a field 17 bytes into a 22 byte VPS and would otherwise have left the SPS updated and the VPS silently unchanged. Verified on a 3840x2160 Main10 stream: the output is byte identical apart from those single bytes, and asking for the level the stream already has produces a byte identical file and no log line.
- Access unit delimiters are now written for HEVC when a source has none, which the H.264 path has done for years. They are optional in HEVC itself, where the standard only requires that one come first in its access unit if present, but on a disc they are not optional in practice: measured across two pressed UHD titles, both video layers of each, every one carries exactly one per picture without exception. A source that omits them was previously authored to a disc without any. The pic_type written declares the set of slice types the picture may contain, so it is derived from the slices actually present rather than assumed; announcing that a picture may hold B slices when it holds only I slices would be wrong. Verified over 741 access units, every value correct, and a source that already carries delimiters is untouched.
- The HDR mastering display and content light level are now repeated at every random access point when authoring a Blu-ray. A pressed disc carries both at every one; material that states them once, at the head of the stream, leaves a player with no HDR10 metadata at all after a seek. Dolby Vision hides the problem, because its own metadata travels with every picture, which is why it is easy to miss. Nothing is fabricated: the bytes written are the stream's own, captured when they first went past, and a source that already repeats them gains nothing. They are inserted immediately before the first slice, by which point every other part of the access unit has been seen, so what the source already provided is known rather than guessed, and the order the standard requires is preserved. Applies to Blu-ray output only.

## tsMuxeR 2.16.0

- Matroska output was losing almost everything except the picture and the sound. Language was never written at all, and since a Matroska file that declares no language is English by definition, every track in every file tsMuxeR had ever written claimed to be English. Chapters were read, listed and then discarded, which loses the 200 to 250 marks of a feature playlist. Dolby Digital Plus was labelled as plain AC-3, and TrueHD as plain AC-3 as well, so a player that trusted the label tried to decode the wrong thing. HDR10 colour, the mastering display values and MaxCLL and MaxFALL were read for the disc path and thrown away on the Matroska one, so an HDR10 source came back as an untagged file. All of it is written now.
- A language the source container already knows is carried over without needing an explicit lang=, in both directions, so a disc authored from a named Matroska track now names it on the disc as well. An explicit lang= still wins, and demuxed file names are unaffected.
- New track-name= on a track, and "default" now takes effect on Matroska output. Without an explicit flag Matroska treats every track as a default track, so a file with two audio tracks declared both of them default. Exactly one track per type is marked now, and the interface can choose which one: its default audio and default subtitle controls previously reached the meta file for disc output only, so on an .mkv the checkbox was present, was settable, and silently did nothing, leaving every player to take the first track.
- Fixed a mux with two video tracks describing itself by whichever video the meta file happened to list last, because only one stream advanced the running duration and that duration becomes the playlist end. On a Dolby Vision disc the second track is the enhancement layer, and the natural listing order puts it second, so the disc claimed the enhancement layer's length and every chapter mark past that point was dropped as out of range, while the feature itself was written in full. Reported by @axledentaldj against 2.15.0. The main stream is now chosen by what the track is, a primary video outranking a secondary or an enhancement layer, rather than by where it sits in the file, and a title runs as long as its primary video does. Reproduced and fixed against a pressed dual layer Dolby Vision disc, where the natural order previously gave 24.8 seconds against a 60 second base layer. A mux with a single video track is byte for byte identical.
- Fixed the file list of a playlist built from several sections showing the second entry with two file names run together, the third with three, and so on. On a 57 section playlist from a pressed disc the last line ran to 602 characters naming 57 files. The GUI builds its file list from those lines.
- A DTS:X track is now named. It used to be shown exactly like a plain DTS-HD Master Audio track, because nothing in the extension substream header says DTS:X: the marker sits at the very end of the XLL band data and is only reachable by walking the asset descriptor, the XLL common header, the channel set sub-headers and the navigation table. A track carrying it now prints " + DTS:X" beside "DTS Master Audio", in the same place a TrueHD track prints "+ ATMOS". Confirmed by walking every frame of all three DTS tracks of a pressed disc: 857,001 of 857,001 frames of the 7.1 track carry the marker and neither 5.1 track carries it. A second marker value, used by some material and matched under a mask because its low nibble is not part of the sync word, rests on two short samples rather than a full disc. This is labelling only; the audio is untouched and produced files are byte-identical.
- Dolby Vision in Matroska, for sources with a SINGLE picture track only. Such a file now carries the configuration record a television reads to recognise Dolby Vision; without it the file played as HDR10 even though the data was there. That covers streaming material, profiles 5 and 8, and it was confirmed on hardware. A DISC IS NOT COVERED AND NOW DELIBERATELY CLAIMS NOTHING: a Dolby Vision disc carries the picture on two tracks, a base layer plus a quarter resolution enhancement layer holding the Dolby Vision data, and Matroska needs the two merged into one track. That merge is not built yet, so the record is not written when a second picture track is present, and the mux says so in its log. Writing it there would leave a 1080p enhancement layer announcing itself as complete Dolby Vision when it is neither complete nor playable on its own. A disc source produces a plain HDR10 file.
- Fixed a track delay of exactly the length of an open GOP's lead-in being reported as zero and then lost on a demux and remux. "Stream delay" was measured against the smallest timestamp on the stream rather than the first one, and on an open GOP the leading B pictures are presented before the first coded picture. Found on AVCHD clips from a Sony ZV-1, whose audio runs 80 ms ahead of the picture; clips from a Panasonic Lumix DMC-TZ110 never reproduced it because their first coded picture is also their first displayed one. Nothing about it is specific to AVCHD; any open-GOP source is affected.
- Fixed the reported and written picture height on sources that pad the coded picture to a round number of rows and mark the extra ones as not for display: a 2160 row picture was described as 2176, and a 1634 row one as 1664. The conformance window is now applied. Discs were always correct and are unchanged. VVC is deliberately untouched, as no test material was available for it.
- Fixed the Dolby Vision level written into a disc being one step too high for material at exactly 24.000 frames per second, caused by a rounding error putting the computed pixel rate three units past a boundary that is defined as an exact rate. Players that support the higher level were unaffected, which is why it went unnoticed, and 23.976 material was never affected.
- A mistyped per-track option in a meta file used to be accepted in silence, so langauge= instead of lang= did nothing and gave no clue why. Unknown option names now produce a warning and the mux continues, so existing meta files keep working.
- --bdmv-to-iso listed nine options in its usage error and only three in --help. The six that were missing (--keep-extra-files, --inner-only, --original-order, --no-layer-fit, --disc-capacity, --label) are now documented, each described from what it does rather than from its name. The GUI already offered them, so only command line users were short of the information.

## tsMuxeR 2.15.0

- Fixed the mastering-display luminance written into a UHD Blu-ray playlist having its maximum and minimum swapped. A disc mastered at 4000 nits was described to the player as 610 nits with a black level of 2.3040 instead of 0.0050. This affects every UHD Blu-ray tsMuxeR has authored from an HDR10 source, not just recent versions. A disc authored with this build now carries the same value as a pressed reference disc. The picture primaries, white point, MaxCLL and MaxFALL were always correct.
- The delay of a demuxed audio track is now recorded in its file name, using the long established DELAY convention: "00294.track_4353 DELAY -17ms.ac3". Muxing a file named that way applies the delay again. A raw elementary stream has nowhere to keep a timestamp, so a track that was offset against the video used to come back at zero after a demux and remux, which is a real loss of sync on discs that use one. An explicit timeshift= on the meta line still wins, and the demuxed audio itself is unchanged.
- Fixed a stream parsing fault that could make a whole H.264 track disappear on the Linux and macOS builds. Two values were read from the bitstream in an order the compiler was free to choose, and reversing them inverted a check that rejects the stream. The Windows builds were never affected.
- down-to-dts on a track with no DTS core, which is every DTS Express stream, wrote an empty file and reported success. In a mux with other tracks it left an entry in the stream table with nothing behind it. It now reports that there is no core to keep. DTS-HD Master Audio and High Resolution tracks are unaffected.
- down-to-ac3 on an A_MLP track was accepted and quietly did nothing. A standalone TrueHD stream has no AC-3 core to keep and tsMuxeR does not encode audio, so it now says so and names the two things that do work, while still muxing the track as before.
- Fixed LPCM tracks differing from their source by one byte. The first frame of every output set a flag that no pressed disc sets. A 16 bit stereo and a 24 bit 5.1 track now reproduce their source disc byte for byte.
- Tracks that cannot be muxed are now named in the interface as well as on the command line. Opening a Blu-ray stream that holds only the disc menu overlay used to add a blank row to the track list with no explanation, and a file containing nothing else reported that its stream type could not be detected, which was not true. The description is translated in all eight interface languages.

## tsMuxeR 2.14.0

- The mux start time is now guarded for Blu-ray outputs (#5, requested by oniiz86 based on Emulgator's doom9 analysis). Muxing has always defaulted to a safe 600 seconds when the start time is left alone, but an explicitly entered value below 524280 ticks of the 45 kHz clock (11.65 seconds) produces a disc on which players cannot navigate back to the start of the stream: they subtract the decoder preroll from timestamps near zero and their 32-bit 45 kHz registers underflow. Twelve commercial discs were measured (BD and UHD, three studios): none starts below 524280, and one studio masters at exactly that value. Lower values are now raised to 524280 with a log message, in the muxer and in the GUI field alike. This applies to the outputs that actually carry the offset, so plain *.ts, Matroska and demux mode are left alone. The 600 second default is unchanged, so existing workflows produce identical results.
- Fixed the TrueHD + AC-3 core merge writing its audio packets with PES stream id 0xBD instead of 0xFD. Blu-ray players route HD audio by the extended id 0xFD plus the extension byte, so strict hardware could fail to pick up a merged track. Plain TrueHD without a core keeps the private-stream id 0xBD it has always used, since it carries no such extension.
- Fixed the TrueHD + AC-3 core merge losing core frames on longer muxes. The merge read its side data misaligned by a reader buffer pad, so each delivered block began with pad garbage and lost its real tail: with merge-ac3-track the core silently stopped a few minutes in, and with merge-ac3-file about 1.6 percent of the core went missing. A full-length UHD test now delivers the complete core to the last frame (195,887 of 195,887 expected AC-3 frames on a 104 minute movie). The result was checked against a pressed disc rather than against earlier output: taking a commercial UHD that carries TrueHD with its AC-3 core natively, splitting the two apart and merging them back together reproduces the disc's own core bit for bit.
- Fixed TrueHD audio being silently thrown away when muxing from a raw TrueHD (.thd) elementary stream. Whenever an audio unit straddled the end of one of the muxer's 2 MiB input blocks, the reader could not tell "I need the rest of this unit" from "this unit is broken", so it reported a bad frame, skipped to the next sync point and discarded everything in between, leaving only a "bad frame detected ... Resync stream" line in the log. On a 20 MiB test slice that silently dropped 53,764 bytes of audio in two places, and the losses repeat about every 2 MiB. The reader now asks for the remaining data instead, and the same input comes back out byte for byte identical. This is the case you get from the usual demux-then-remux workflow.
- Fixed a start code scanner defect that could mis-detect frame boundaries on builds without SSE2, which includes the macOS arm64 binary published with each release. A payload byte 0x01 immediately before a four byte start code could make the scanner return a pointer one byte early, affecting every video codec. Builds with SSE2, which covers all Windows binaries, were never affected.
- Fixed the last frame of a down-converted DTS track carrying a leftover DTS-HD block. When a DTS-HD Master Audio or High Resolution track is stripped down to its DTS core, the final frame was written out with its HD extension still attached, because a stream ending exactly on a buffer boundary was mistaken for one that still had data to come. Muxing DTS tracks without down-converting was never affected.
- Down-converting a Dolby Digital Plus track that has no AC-3 core now fails with a clear message instead of writing an empty file and reporting success. Extracting the core works by keeping the AC-3 frames and discarding the E-AC-3 ones, so a DD+ stream with no core at all, which is what streaming sources normally provide, had nothing left to keep.
- Fixed a crash ("Bitstream exception") that could abort a mux, or an analyze run, at a fixed position in some HEVC streams. One malformed metadata (SEI) unit was enough to stop the whole run even though that data is only analyzed, never modified. Such units are now skipped with a warning.
- Dolby Vision is now reported against the track that actually carries it. On a dual layer Dolby Vision disc the two HEVC tracks looked identical apart from their resolution, with nothing to say which one held the Dolby Vision data. The track listing now marks the stream carrying the Dolby Vision RPU, while the base layer stays plain HDR10.
- Tracks that cannot be muxed are now named instead of reported as a blank failure. A Blu-ray stream containing an interactive menu used to print only "Can't detect stream type" against that track. It now says that the track is an Interactive Graphics stream, the disc's menu overlay, that muxing this stream type is not implemented, and that the track is skipped. Text subtitle tracks are named the same way, and any other unrecognised track from a transport stream at least reports the stream type the container declared. The GUI track list is unchanged: unsupported streams are still not offered for muxing.
- GUI: the Dual-layer (BD-R/RE DL) controls now have explanatory tooltips, translated in all eight interface languages, and they follow a runtime language change like the rest of the window.
- Internal rework of how streams are scanned and buffered, with no change to what comes out. The AC-3 side track of the TrueHD merge is no longer re-copied for every frame it hands over. The start code scanner that every video codec funnels through now uses the CPU's vector instructions where they are available, a redundant second scan of large video frames was removed, large video frames are packetized straight into the output buffer instead of passing through a staging buffer, and a few repeated per-packet lookups were hoisted out of the mux loop. Output is byte-for-byte identical to the previous release across the whole test set, including subtitles, split output, CBR and plain TS. No speed figure is quoted: how much difference this makes depends on whether the CPU or the drive is the limiting factor, and on some measurements the two builds sit within run-to-run noise of each other.
- Hardened several long-standing edge cases found while reviewing the above: a malformed stream can no longer drive the output buffer into an endless growth loop, a TrueHD stream that never decodes a sync header can no longer divide by zero, an MLP header field is now range checked before it is used to walk a buffer, and merging from an AC-3 file with no sync word in its first block no longer discards a whole block of audio while reporting success.

## tsMuxeR 2.13.3

- Fixed the interface language not fully switching on the BDMV to ISO tab. Changing the interface language at runtime did not re-translate the "Include all files from the folder", "Keep data on the inner disc area", and disc label controls, so they kept the previously shown language while the rest of the tab updated. They now switch with everything else. This became visible only once 2.13.2 fixed the embedded translations.

## tsMuxeR 2.13.2

- Fixed the interface translations. A build-pipeline issue had been embedding a stale compiled-translation file, so several BDMV to ISO options added since 2.11.0 - the disc label field, "Include all files from the folder", and "Keep data on the inner disc area" - appeared in English in every language. The build now always embeds the current translations, so all eight interface languages show correctly again.
- The "Include all files from the folder" tooltip now also explains that the full standard Blu-ray folder structure is completed automatically, translated in all eight languages.

## tsMuxeR 2.13.1

- BDMV to ISO: corrected the "Include all files from the folder" checkbox tooltip. It now states that the full standard Blu-ray folder structure (the empty AUXDATA, BDJO, JAR and META folders, a CERTIFICATE folder, and a populated BACKUP) is completed automatically by default, matching a real BD/UHD disc. No behaviour change; the 2.13.0 build already created these folders.

## tsMuxeR 2.13.0

- GUI (BDMV to ISO): new **Keep data on the inner disc area (pad the outer edge)** checkbox (CLI: `--inner-only`). The outer edge of an optical disc is the most error-prone part to burn, so this packs the movie onto the inner tracks of every layer and fills the outer/rim region with zeros, keeping the data off the weak outer edge. The layer-break guard is sized automatically from the disc type and the content, and the image is padded to the full disc. When it is on, the manual guard controls are greyed out. Verified on dual-layer BD-R DL. Requested by DreckSoft.
- Tested on marginal media: a BD-R DL that verified with a 41 MB uncorrectable defect right at the layer transition still played the movie flawlessly (checked across 1:29 to 1:31), because the defect fell entirely inside the guard's zero band, so no video data was lost.
- BDMV to ISO: the image now gets the full standard Blu-ray folder structure by default (the empty AUXDATA, BDJO, JAR and META folders, a CERTIFICATE folder, and a BACKUP populated with copies of index.bdmv, MovieObject.bdmv and the PLAYLIST/CLIPINF files), the same structure tsMuxeR already creates when it authors a disc, so players that expect the complete layout are satisfied. Only what the source is missing is added: a folder that already ships its own BACKUP is left untouched, and the large .m2ts streams are never duplicated into it. Requested by Oleekae.
- BDMV to ISO: images build faster and use far less scratch space. The layer-break and inner-only guard padding is now stored as a sparse region instead of writing gigabytes of literal zeros to disk, so a guarded image finishes much quicker while burning byte-for-byte identically (an 8 GB inner-only guard costs no disk space and no write time). The folder copy uses a larger buffer as well.

## tsMuxeR 2.12.0

- GUI (BDMV to ISO): new "Disc label (optional)" field (CLI: --label=<name>). The volume label is written into the ISO, the same as the direct ISO output already allows; leaving it empty keeps the previous behaviour. Requested by DreckSoft.
- GUI (BDMV to ISO): new "Include all files from the folder (not just BDMV)" checkbox (CLI: --keep-extra-files). By default only the disc-structure folders (BDMV, CERTIFICATE, AACS) are written; ticking this also adds every other file and folder next to BDMV (readme files, cover art, extra folders) to the image. Requested by DreckSoft.
- Both new options are off by default, so an unchanged workflow produces the same ISO as before, and both are translated in all eight interface languages.

## tsMuxeR 2.11.0

- GUI (BDMV to ISO): the layer-break guard now defaults to 288 MB. Reported real defect zones cluster around 35, 64 and 258 MB, and on a defect-managed disc the true layer switch can sit up to 128 MB after the calculated break; 288 covers all of these. The coloured hint under the field explains the choice at every value. Requested by Coopervid, based on his defect-size reports.
- The small guard before the break now scales proportionally with the main guard: one sixteenth of the after value (the 64:4 ratio of the original design), at least 4 MB, so 288 gives 18 MB before. The advanced field shows the live value and stops following the moment you set your own. Suggested by Coopervid.
- Layer-fit placement (automatic): when the file that would cross the break fits completely on the next layer, it is placed there whole, so the break falls cleanly between two files. Two big titles (theatrical and directors cut) get one layer each; on seamless-branching discs the break lands between segments, the same spot commercial authoring uses. A movie larger than a layer still straddles the break with the guard, as before. Disable with --no-layer-fit.
- New checkbox "Keep original file order (seamless branching)" (CLI: --original-order): keeps the many segment files of a branching disc in their playback order instead of largest-first.
- Layer break report: after every build, the log shows where each guard landed, and for the main movie the playback time of the break, so you know where to spot-check on a player. The same information is saved next to the image as name.iso.layerbreak.txt. On seamless-branching discs the time is relative to the named segment file.
- CLI: --disc-capacity=<sectors> lets the layer-fit placement respect the disc end; the GUI passes it automatically.
- Selecting the BDMV folder itself (instead of the disc root above it) now automatically steps up to the parent, in the GUI and the CLI. Reported by Coopervid.
- The folder row has a refresh button, and re-picking the same folder re-reads it, so a fit estimate can no longer go stale after files were removed. Reported by Coopervid.
- The suggested output ISO now goes to the parent of the source folder, and the build only ever includes the disc structure folders (BDMV, CERTIFICATE, AACS); anything else next to them, such as a previously built ISO, is skipped with a log line instead of being muxed in.
- Long tooltips now word-wrap instead of rendering as one endless line.
- The playback-time reader samples several positions and uses the median, so a small block with a foreign timeline inside a stream can no longer produce a wrong time.
- The disc-authoring guide (English, German, Japanese) covers all of the above, plus new sections on seamless branching and defect-managed discs (measured on real hardware).

## tsMuxeR 2.10.1

- GUI (BDMV to ISO): the layer-break guard field now accepts up to 9999 MB (was 1024), for media whose defect zone at the layer transition is much larger than the ~35 MB typical case. The size hint now says so and suggests raising the guard if a test burn fails just after the break. Reported by Coopervid.

## tsMuxeR 2.10.0

- GUI (BDMV to ISO): the disc-type list now has separate BD-R DL and BD-RE DL entries and pre-fills the disc's standard "Free Sectors" for the chosen media, so you no longer need to run ImgBurn for a standard disc. The field is locked to prevent accidental changes; tick "Enter Free Sectors manually (advanced)" to override it for a non-standard disc (a reformatted BD-RE, or a BDXL burned without defect management). The pre-filled capacities were read from real Verbatim discs and match the Blu-ray/BDXL spec. Requested by Coopervid.
- Windows: fixed the GUI failing to start on Windows 7 with "api-ms-win-core-winrt-l1-1-0.dll is missing". The release build was shipping an unpatched, Windows 8+ Qt6 because a stale build cache skipped the qt6windows7-patched rebuild; the cache is now keyed so the patched build runs, and CI fails if a WinRT (Windows 8+) import ever returns to the shipped Qt. Reported by Coopervid.
- CI: updated actions/checkout to v5 (Node.js 20 is deprecated).

## tsMuxeR 2.9.6

- BDMV to ISO: report copy progress so the GUI progress bar advances while the ISO builds. The build itself is unchanged; it can simply be slow when the source is an optical disc (a few MB/s), and previously the bar sat at 0.0% the whole time and looked hung. tsMuxeR now prints "percent complete" as it copies, so the bar sweeps from 0 to 100. Reported by Coopervid

## tsMuxeR 2.9.5

- GUI: the "BDMV folder to ISO" tab now shows immediate feedback when you pick a folder: whether a BDMV was found, and for a read-only disc (a mounted ISO or optical drive) a note that the output ISO must go to a writable location. The folder size is also read instantly for discs (from the volume) instead of walking every file, so selecting an optical drive no longer stalls. Reported by Coopervid

## tsMuxeR 2.9.4

- Fix reading a BDMV from a mounted ISO or optical disc. The recursive directory scan tested the directory attribute with XOR, so on read-only media (where folders also carry the read-only and archive attributes) it skipped every folder and reported no files. Reported by Coopervid
- GUI: when the input BDMV is on read-only media such as a mounted ISO, default the output ISO to a writable folder instead of the read-only drive, and warn before building if the chosen output is not writable. Reported by Coopervid

## tsMuxeR 2.9.3

- GUI: hide the mux controls (output type, file name, meta file, and the mux and meta buttons) while the "BDMV folder to ISO" tab is active. That tab is self-contained and uses its own Build ISO button, so the mux controls were irrelevant and confusing there. Reported by Coopervid

## tsMuxeR 2.9.2

- GUI: add a tooltip on the layer-break guard field explaining that the burned zone aligns to whole 2048-byte sectors and file boundaries, so it matches the entered value within about 1 MB
- CI: name the release assets tsMuxeR-<version>-<platform>.zip instead of w64.zip / lnx.zip / mac-<arch>.zip

## tsMuxeR 2.9.1

- GUI: move the dual-layer controls (Fit to disc, Layer-break guard, Allow oversize) out of the progress dialog into the main window Output group, and show them only for Blu-ray ISO or Blu-ray folder output (hidden for TS, M2TS, MKV, AVCHD and Demux)

## tsMuxeR 2.9.0

- `--layer-break-guard-before=<MB>`: size the guard zone before the layer break on its own, instead of the fixed 4 MB margin, for media that are defective on both sides of the transition. The default stays asymmetric (the measured defect sits at the start of the next layer)
- GUI: the "BDMV folder to ISO" tab gains an optional "fill before the break" control with a short note on when to use it, and a live fit estimate that shows the image size against the disc's Free Sectors and whether it will fit

## tsMuxeR 2.8.1

- GUI: show the version number in the window title (previously only the git commit was shown)
- GUI: stop the "#" column header from overlapping the select-all checkbox in the track list

## tsMuxeR 2.8.0

Multi-layer disc authoring, on top of jaminmc/tsMuxer. See docs/DISC_AUTHORING.md.

- `--bdmv-to-iso`: wrap an existing unprotected BDMV folder into a burnable BD-ROM ISO byte for byte, keeping the BD-J menus and all clip and playlist references intact (no re-mux, no re-numbering)
- `--layer-break-guard=<MB>`: zero-fill the defect-prone sectors around each layer transition of BD-R/RE DL and BD-R XL media, so the movie plays across the break. Validated on real hardware, where it absorbed a genuine layer-1 defect
- `--layer-break-lbn=<sector[,sector...]>`: set the layer break sector(s); takes a comma list for 100 GB (2 breaks) and 128 GB (3 breaks) BD-R XL
- `--disc-size` and `--allow-oversize`: abort, or warn, before muxing if the estimated image will not fit the target disc
- ISO writer: multi-sector directories and 32-bit object IDs, so full retail discs (hundreds of files) can be wrapped
- Propagate asynchronous write errors so a truncated disc is no longer reported as a successful mux; fix a buffer leak on the write path
- Skip unsupported subtitle coding types in the Blu-ray playlist with a warning instead of aborting a finished, multi-hour mux
- Guard a divide-by-zero in the discovery phase, and stop a bogus "MLP is not standard" warning when a TrueHD track is merged with an AC-3 core from a file
- GUI: a "BDMV folder to ISO" tab with a layer-break calculator (paste the disc's Free Sectors from ImgBurn and the break sectors are worked out), colour-coded guard hints, disc-type and divisibility sanity warnings, and a BD-R XL at-your-own-risk confirmation
- GUI: dual-layer capacity and guard controls on the Blu-ray outputs, and runtime language switching that also updates the hand-built widgets
- GUI: added Japanese, and completed German, Spanish, French, Hebrew, Russian and Chinese to full coverage
- Windows: the release binaries build Qt 6.8.3 statically with the qt6windows7 patches, so the standard 32-bit and 64-bit builds run on Windows 7 and newer. This replaces the earlier separate Qt5 build. The workflow builds Qt, qttools and a static zlib with Ninja

## tsMuxeR 2.7.2
- **Qt6 GUI:** The default GUI build uses Qt6. In this fork the Windows binaries are built with the qt6windows7 patches, so they still run on Windows 7 (see docs/COMPILING.md)
- Added language selection to the video track options in the GUI, matching the existing audio/subtitle language selector
- Fixed AAC audio not being detected in MP4/MOV containers (reported as "Can't detect stream type"), caused by missing ADTS header generation when ESDS parsing did not set the AAC flag, and by channel count corruption from the AudioSpecificConfig channel configuration index
- Fixed MKV and MOV/MP4 demuxers returning absolute timecodes instead of relative delays, causing audio/video sync offsets (e.g. delay stored in differing track start times or edit lists) to be lost during remuxing
- GUI: Make meta file editable with manual override support, allowing users to manually edit the meta file directly in the GUI and apply advanced options not available through the visual interface
- GUI: Added "Reset meta to auto-generated" button to revert manual meta edits
- GUI: Support for merge-ac3-file option for TrueHD tracks, enabling TrueHD+AC-3 merge in the GUI with external .ac3 file input
- GUI: Preserve custom Blu-ray chapters when input files change
- Introduced AV1 codec support in MPEG-TS, implementing the AOM "Carriage of AV1 in MPEG-2 TS" draft specification
- AV1 muxing from MKV and MP4/MOV containers into MPEG-TS with start-code based OBU format and emulation prevention bytes
- AV1 demuxing from MPEG-TS to raw .obu elementary stream files
- AV1 Sequence Header parsing for profile, level, resolution, bit depth, color primaries, transfer characteristics, and HDR/WCG detection
- AV1 PMT descriptors: Registration descriptor (format_identifier 'AV01') and AV1 video descriptor (tag 0x80) per AOM spec
- AV1 keyframe detection for random access point signaling in MPEG-TS adaptation field
- Introduced automatic FPS detection from container metadata (MKV default_duration, MP4/MOV timescale) for codecs that lack timing info in the elementary stream (e.g. AV1)
- Fixed MP4/MOV files larger than 4GB not being read correctly due to compact atom size overflow
- Fixed AAC MPEG-2 TS descriptor (was disabled with early return; now emits proper AAC descriptor tag 0x2B)
- Fixed TrueHD/MLP TS descriptor to emit Blu-ray compliant HDMV registration descriptor in Blu-ray mode, or SMPTE-RA 'mlpa' registration descriptor otherwise
- Updated GUI to recognize AV1 codec: file dialog filters (.obu), About dialog, BD V3 auto-selection, and video track settings
- Updated all GUI About dialog translations (EN, DE, ES, FR, HE, RU, ZH) to include H.266/VVC and AV1 in supported codecs list
- Introduced Matroska (MKV/MKA) muxing support with EBML writing, cluster-based output, Cues index, and SeekHead
- Supported codecs for MKV output: H.264, HEVC, VVC, AV1, VC-1, MPEG-2 (video); AAC, AC3, E-AC3, DTS, TrueHD, LPCM, MP3 (audio); SRT, PGS (subtitles)
- Added CodecPrivate generation for H.264 (AVCDecoderConfigurationRecord), HEVC (HEVCDecoderConfigurationRecord), AV1 (AV1CodecConfigurationRecord), AAC (AudioSpecificConfig), and VVC
- Added MKV radio button to the GUI output panel with save dialog filter and auto-detection of .mkv/.mka extensions
- Updated all GUI translations (DE, ES, FR, HE, RU, ZH) with MKV muxing strings
- Added missing Matroska codec ID constants (A_DTS, A_EAC3, A_TRUEHD, A_MPEG/L3, V_MPEG2)
- Fixed aspect ratio override (`ar` parameter in metafile) being ignored for MPEG-2 and other video streams, so the original stream aspect ratio was always retained
- Added Opus audio codec support for MPEG-TS and MKV muxing, including OpusHead codec-private handling and TS descriptor generation per RFC 7845
- Added FLAC audio codec support for MKV muxing and demux output, including STREAMINFO codec-private parsing
- Introduced a stream discovery phase that probes all tracks before muxing begins, collecting codec properties (channels, sample rate, resolution, HDR metadata, codec-private data) upfront to prevent late-initialization bugs in container headers
- Generalized early codec-private propagation from containers (MKV, MP4/MOV) to all stream readers via `applyDiscoveryData()`, replacing the previous Opus-specific workaround
- Added `getTrackCodecPrivate()` support for MP4/MOV containers
- Added FLAC and Opus codec entries to USAGE documentation

## tsMuxeR 2.7.1
- Fixed file dialogs not appearing on macOS with Qt6 by using non-native dialogs
- Fixed browse button for output folder passing wrong parameter to file dialog
- Consolidated file type filters in add dialog to reduce dropdown size
- Added automatic copying of tsMuxeR CLI into macOS app bundle during build

## tsMuxeR 2.7.0
- Fixed a an issue so that Dolby Vision EL stream type is now correct 
- Fixed a bug with HEVC streams when an HDR10+ SEI payload is too short
- Fixed a bug where the first 2 frames of the first video track are muxed before anything else
- Introduced an improvement so Single Track Double Layer files now can properly handled
- Fixed a bug so that if no MOOF atom is met we stop atom parsing at the next MDAT atom
- Fixed an issue with HDR flags, so we only set them if an HEVC stream is detected
- Introduced correct ATSC descriptor for pure EAC3 tracks
- Introduced correct HDMV TS descriptors for MPEG-2 streams
- Fixed an issue where Blu-Ray movies will loop rather than stopping after reading
- Introduced being able to include Dolby Vision descriptors in TS or M2TS mode
- Fixed the order of streams so that video streams always come first
- Introduced a GUI option for adjusting PIP transparency
- Fixed an issue where translated strings appeared in the meta file
- Fixed a bug in the output paths of the MXE build scripts
- Ensured we keep M2TS descriptors in TS files (temporary until a long-term solution can be found)
- Fixed a bug where filenames were being truncated prematurely if there were dots in the filename
- Introduced putting overnight builds into OBS, to build for various Linux platforms
- Improved the documentation to fix a broken URL for the test files
- Introduced a simplification of the method used to play sounds in the GUI
- Fixed an issue with broken ISO labels when using non-ASCII characters
- Introduced a refactoring that moved the About page into an external HTML file for the GUI
- Introduced a code cleanup that removed all usages of std::wstring
- Fixed an issue with incorrect subtitle spacing on Windows
- Introduced support for M4V files
- Fixed issue with subtitle timestamps when joining multiple M2TS files together
- Fixed incorrect usage of POSIX APIs in Windows builds
- Fixed a bug with encoding errors when dealing with SSIF files
- Fixed a bug where we could read over the end of an MP4 file
- Introduced keeping the track order when multiple video tracks are added
- Introduced support for reading fragmented MP4 files
- Introduced support for specific AVC and HEVC descriptors in TS files
- Introduced support for Dolby Vision atoms in AVC or HEVC streams
- Introduced a changelog and improved general documentation
- Fixed an issue with garbled subtitles being displayed
- Introduced translation support, as well as a full Russian translation of the GUI
- Introduced getting the HDR10 information from the SPS VUI in HEVC
- Introduced detection of UTF8 in subtitle files
- Fixed usage of WinMain, which lead to issues with console output on Windows
- Introduced converting meta files using active code page if UTF8 fails
- Improved the documentation for building with Msys2
- Fixed bugs in the handling of non-ASCII characters in paths on Windows
- Fixed bugs in subtitles PIDs for BD V3 M2TS with HDR
- Fixed bug with the display of bitrate and channel numbers for EAC3 and AC3 tracks
- Fixed bug with GUI not correctly allowing to select DTS Express 24-bit as a secondary track
- Introduced an error message when an output file longer than 255 characters and reduced overall file length
- Fixed bug where 3D plane information was showing for 2D BD-ROMs
- Fixed a bug with uneven width between characters in subtitles on Mac and Linux
- Introduced the ability to detect audio delays in MKV files
- Fixed a bug where the 3D planes were not detected in specific cases
- Fixed a bug with alignment of the subtitle tracks and 3D planes
- Removed unnecessary floating point conversion code from the GUI source tree
- Added support for frame rates of 50, 59.94 and 60
- Fixed an issue with HDR10 HEVC streams where the maxCLL and maxFALL values were set incorrectly
- Fixed typos and improved the clarity of certain wording in the GUI
- Fixed typos and grammar issues with the readme and usage information
- Introduced the git revision to the version string in the GUI and CLI
- Introduced automatic selection of BD V3 for HEVC in GUI
- Fixed an issue with compiling on Mac
- Fixed an issue with the handling of wav64
- Introduced a workaround for QTBUG-28893
- Performed another round of GUI code cleanup
- Introduced a uniform code formatting style
- Fixed a bug with reading the FPS information from certain streams
- Fixed a typo in the GUI settings for the font family setting
- Introduced a warning when a V2 video format is used for a V3 Blu-ray
- Fixed a bug with incorrect stream ID for TS stream
- Fixed typos in the source files
- Introduced UHD Blu-ray as an option in the GUI
- Fixed a bug where invalid font files could crash tsMuxer
- Fixed an issue with HEVC stream detection in the GUI
- Introduced reading the FPS info from VPS or SPS, rather than VPS only
- Fixed a bug with the CPI table I-frame thresholds with UHD
- Introduced Dolby Vision support
- Fixed compiler warnings on return value overflows
- Fixed an issue with the stream ID being incorrectly set for BD V3
- Fixed an issue when spaces where in the path to the temporary meta file in the GUI
- Fixed an issue with buffer overflows on HEVC streams
- Fixed an issue so that TS descriptors are the same as on commercial Blu-rays
- Fixed an issue where numbers were shown instead of language codes in the GUI
- Introduced nightly builds, hosted on Bintray
- Fixed a bug where the tsMuxer executable could not be found on Windows in the GUI
- Fixed a bug where muxing a SRT results in a segfault on Linux
- Introduced support for UHD HDR10 and HDR10+
- Introduced a migration from "override" to "virtual" keywords in the code to conform better to C++14
- Introduced a migration from "QObject::connect" syntax to Qt5 equivalent in the GUI
- Fixed an issue with the min and max functions when compiling on Windows
- Fixed an issue calculating the AAC frame size
- Introduced UHD (width >= 2600) support in the MPLS and CLPI
- Introduced a clean up and reformatting of the documentation
- Introduced UHD BD V3 support
- Fixed an issue with EAC3 bitrate, sampling rate and channel information not being set correctly
- Fixed a bug with parsing of AC3
- Fixed an issue with the stream type not being set correctly for H265
- Fixed an issue when parsing MP4 AAC 5.1 where the channel output is not read correctly
- Fixed an issue with parsing the AAC frame length
- Introduced an update of the C++ standard from 11 to 14
- Introduced a cleanup of precompiled headers
- Introduced using std::thread for the TerminatableThread in libmediation
- Introduced cross-platform CMake build system
- Introduced a cleanup of libmediation that removed condvar, mutex and time from the library
- Introduced a translation of comments from Russian to English
- Introduced a migration from Qt4 to Qt5

## tsMuxeR 2.6.15
- Fixed mkv parser a bit. I've got unparsed file example

## tsMuxeR 2.6.13
- update SEI correction: do not correct SPS/PPS if stream contains different PPS with same pps_id

## tsMuxeR 2.6.12
- several minor bugs fixed

## tsMuxeR 2.6.11
- fixed saving UI settings to a registry. Also, if file tsMuxerGUI.ini found, UI will switch settings to an ini file instead of registry
  (you can create empty ini file at the beginning).
- UI: change control for cut start/end time
- fixed SEI processing for 'force' mode ( it doesn't work correctly for some movies)
- fixed bug in the wav demuxer (first audio frame has mixed up channels)
- fixed timings for PG streams. Timings was inaccurate for amount of several ms (for some movies only, it depended of the first PTS of the file)

## tsMuxeR 2.6.9
- inserting SEI did not work for some H.264 stream at all
- add more correction for VUI parameters if option insert SEI is active (it helps to open some H.264 streams in the Scenarins
  and solve PS3 problem for some sources)
- fixed channels for 7.1 and 7.0 wav files
- fixed combined H.264 streams read from Elementary Stream
- BD Bitrate control improved a little bit

## tsMuxeR 2.6.4
- Add secondary video support
- fixed mp4 files with MPEG-DASH
- fixed SEI again
- fixed DTS-ES recognition
- fixed font renderer (a little bit wrong text position)
- several minor improvments and bug fixes

## tsMuxeR 2.5.7
- fixed bug with SEI messages for some movie
- fixed problem with some movies where problem occured during processing several last video frames
- several minor bug fixes

## tsMuxeR 2.5.5
- add HEVC video codec support
- UI improvment: Save settings for General tab, Subtitles tab and last output folder
- Fixed file duration detection for ssif and some m2ts files
- Fixed bug if mux playlist and several sup files (it is a very olg bug, but it became much more often since 2.4.x)
- Several minor bug fixes

## tsMuxeR 2.4.0
- Add secondary audio support for bluray muxing. Due to standart It is allowed only for DTS-Express and DD+ codecs.
- Filter out H.264 filler packets
- UI improvment: option for MPLS offset can be entered either as time or as 45Khz clock value
- UI improvment: UI displays opened file duration
- UI improvment: chapter list correctly updated if join several files. Also joining for MPLS is enabled.
- Add help if run tsMuxeR without parameters
- Fixed muxing for 96Khz TRUE-HD tracks
- PCM inside VOB was anonced before, but actually did not work. Fixed.
- UI fix: if open MPLS, then close, track list is not cleared. It is broken in previous build only.
- Subtitles renderer fixed (broken in previous build only after in/out effects)

## tsMuxeR 2.3.2
- Support PG subtitles inside MKV
- Support MKV tracks with zlib compression
- Support 3D MP4 and MOV files (combined AVC+MVC stream)
- Add option 'line spacing' to subtitles renderer
- Add fade in/out effect to subtitles renderer
- Fixed ability to drag&drop files directly to tsMuxerGUI shurtcut (it worked before in version 10.6)
- Fixed splitting operation if no video track present
- bug fixed: tsMuxeR can't create output directory for UNC path (for instance \\.\Volume{E5FB13D8-5096-11E3-B9C4-005056C00008}\folder1\test.ts)
- bug fixed: message "file already exist" appeared if open several files from a folder with '(' in the name

## tsMuxeR 2.2.3
- Add support for DTS-HD elementary stream with extra DTSHD headers
- Add support for mkv with 'Header Stripping' compression
- Add 3D MKV support
- Add PCM inside MKV support
- Add PCM inside VOB support
- Fixed option 'bind to video fps' for subtitles
- Improved font renderer quality
- Fixed file splitting option (it was disabled since v.1.11.x because of was not implemented for ISO and 3D-blurays)
- Several minor bug fixes

## tsMuxeR 2.1.8
- Fixed join files problem with True-HD track
- introduce MAC build

## tsMuxeR 2.1.6
- Add support for combined AVC+MVC streams
- Output file size slightly reduced
- Fixed bug if mux AVC+MVC tracks to m2ts file. Some 3d m2ts movies did not play on Samsung Smart TV
- Fixed minor bug in a SSIF interleaving for some movies
- introduce Linux build

## tsMuxeR 2.1.4
- Same problem fixed again. Sometimes tsMuxeR get access to file with wrong name during mpls processing.

## tsMuxeR 2.1.3
- Previous version introduce a new bug. Sometime tsMuxeR showed error message "file not found". Fixed.

## tsMuxeR 2.1.2(b);
- fixed bug in MVC stream recognition. MVC from Intel Media Encoder now work.
- SSIF files is not required any more if you open 3D MPLS file
- Add Stereo subtitles basic support. If source PG stream has stereo format, same stereo PG stream will be created in a output file
- Add tag <force> (or <f>) to srt parser. This tag force to show subtitle message. For instance:

	1
	00:00:10,440 --> 00:00:20,375
	<force>	
	<b>Senator</b>, we're making
	our final approach into Coruscant.

## tsMuxeR 2.0.8:
- fixed subtitles bug: "3d-plane" option was inaccessible for many disks

## tsMuxeR 2.0.7:
 improvments:
- add control for select/unselect all tracks at once
 bug fixes:
- extract ac3 core from e-ac3 track fixed
- fixed option --m2tsOffset (was broken in version 2.x.x)
- fixed 'bufer overflow' error message if simultaneously mux several m2ts files and one of them has PSG tracks only
- fixed problem with too long file names in demux mode for large mpls files

## tsMuxeR 2.0.6:
- bug fixed: removing overlapped frames for HD audio fixed

## tsMuxeR 2.0.5:
- add direct ISO output

## tsMuxeR 1.12.10:
- fixed H.264 stream parser. Same fix as in previous version but more careful
- fixed subtitles color selection in UI

## tsMuxeR 1.12.10:
- fixed H.264 stream parser. It cause video distortion for some movies.
- add DTS-express support. Is not fully complete yet, tsMuxeR doesn't produce subpath for secondary audio

## tsMuxeR 1.12.9:
- fixed file join for mov/mp4
- fixed bug in SEI unit processing (if enable options 'insert picture timing'). Bug may cause video distortion.
- fixed distortion for VC1 codec if join several files
- seamless audio fixed. Extra audio frame correctly removed.

## tsMuxeR 1.12.6:
- fixed 3d subtitles. Add ability to select 3D offset plane for subtitles
- add new parameter '--start-time'. This parameter define time for first video frame in output file. This parameter is filled automatically (too keep same input time) if open MPLS file.
- several more minor fixes in transport stream to improve Blu-ray compatibility
- fixed E-AC3 codec

## tsMuxeR 1.12.3:
- fixed problem with ssif muxing
- add addition check for 'insert picture timing' parameter. For MVC depended view used same value as for primary video stream
- add new parameter to GUI and tsMuxeR core: 'right-eye'. Parameter is used for 3D blurays only. If parameter is set then MPEG-4 MVC Base view video used for Right eye.
This parameter filled automatically in GUI if open MPLS file.

## tsMuxeR 1.12.2:
- add 3d bluray support. Bluray muxing activated automatically if MVC substream appears in input tracks.
To reduce HDD space, tsMuxeR doesn't produce ssif file, only a couple of .m2ts files. ssif files can be 
creted on the fly in DVD fab using "create mini iso" menu item.
- add ability to mux to ssif file directly. It is not supported in GUI, but you can provide .ssif file extension
- fixed bugs in SEI message processing and add MVC sei message support
- fixed several bugs in the Transport Stream to improve compatibility with Blu-ray standart.

## tsMuxeR 1.11.6:
- fixed bug in SSIF file demuxing. It cause a problem for subtitles tracks.

## tsMuxeR 1.11.5:
- added SSIF files support for blu-ray play lists (MPLS)

## tsMuxeR 1.11.4:
- detect language for audio/subtitle tracks fixed for SSIF files (it's work if ssif file is opened from Blu-ray disk structure)

## tsMuxeR 1.11.3:
- bug fixed in MVC parsing

## tsMuxeR 1.11.0:
- add support of SSIF files and MVC codec (3d Blu-ray compatibility)
