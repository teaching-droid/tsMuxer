## tsMuxeR 3.0.0

This release is about MP4 and MOV. tsMuxeR now reads much more of what these files can hold: more codecs, track names, chapters, cover pictures and colour data, fragmented files, joins, and damaged or unusual files. It still does not write MP4. ASS and SSA subtitles are now drawn with libass, with their styles and animations. The changes are grouped below by area.

**Why 3.0.0: two changes can need a look at existing meta files.** ASS and SSA subtitles that are drawn into pictures are now drawn with libass, as their script says, and the font parameters on their line (font-name, font-size and the others) no longer apply to them. Add `ass-as-srt` to the line to draw them as before, as plain text with those parameters. And a demux with `keep-text` writes ASS, SSA and WebVTT subtitles from a Matroska file as .ass, .ssa and .vtt files, in their own form, where it wrote an .srt file.

**The Intel macOS build has been back since 2.19.0, and Mac users are still asked to test.** It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. On any Mac, please also report whether ASS subtitles are drawn with libass after `brew install libass`, and what happens when you change the subtitle font and colour in the window.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

### Worth acting on

* **A Blu-ray playlist muxed each clip whole, also where it plays only a part of it.** The pictures a disc never shows were in the output, a clip played in several parts came in that many times, and the length and the chapters did not match what was muxed: a path of three items through three clips came out 16 times as long. A playlist now muxes only the parts of its clips that it plays, from the key picture at or before the point where a player starts to the one at or after the point where it stops. `--playlist-clips=whole` muxes the clips whole, as before, also in a listing; in the window it is the box "Mux only the parts of a playlist's clips that it plays" in the General box. Mux such playlists again.

* **Interlaced H.264 coded as frames with the bottom field first was muxed at twice its length,** so it played at half speed. Each frame now lasts one frame. Mux such files again.

* **HEVC on a disc made from an MP4 or Matroska file could not be started anywhere but at its beginning.** Such a file holds the parameter sets of its video once, in its header, and the disc got them only at its first picture, so a chapter jump or a seek decoded no picture at all. A disc now gets them at every IRAP picture that lacks them, as the Blu-ray format has them; H.264 already got them. Make such discs again.

* **A WAV with 3 to 7 channels put some of them on the wrong speakers on a disc.** In 2.1 and 3.1 the low-frequency channel went to a surround speaker, in 4.0 the centre went to the left surround, and in 4.1 and 6.0 the low-frequency and the back channels were swapped with others. Each channel now goes to its own speaker: a layout a disc has no code for, such as 2.1, is written as 5.1 with silent channels, 4.0 as 3/1, and a layout a disc cannot hold is refused. Stereo, 5.1 and 7.1 are written as before. A demux of a disc's 2/1 and 3/1 audio now names the back speaker.

* **A raw audio file could be listed as AV1 video,** with a picture size read out of audio bytes, and a mux of it as AV1 said it had succeeded. This happened to AC-3, E-AC-3, DTS and PCM files with a few bytes between their frames. A raw file is now AV1 only when it begins as AV1, and a V_AV1 line over data that is not AV1 is refused. (#48, thanks to @Moonghost-coder for the detailed report)

* **AC-3 and E-AC-3 encoded with timecode were not recognised.** With their timecode option on, encoders put 16 bytes in front of every frame, or every pair of AC-3 core and E-AC-3 frame. That block is now left out: the audio muxed is byte for byte that of the same file made without timecode. (#47, thanks to @Nandos64 for the samples and to @Moonghost-coder for the analysis)

* **An audio or video track that gave nothing ended "successful".** A meta line naming a codec the file does not hold left an output without that track, or an empty one, with only a warning and exit code 0. Such a job, and one whose audio was mostly unusable as the codec named, now ends with an error before success is said.

* **MPEG-2 video with repeated fields (soft pulldown) got uneven picture times:** some pictures got the time of the one before them, others left a gap of two. Each picture now lasts the two or three fields it is shown for. MPEG-2 with a second I picture in a group of pictures is timed right as well.

* **--split-duration split one group of pictures late:** 2 s gave parts of 3 s. A key picture on the duration now starts the next part.

* **Start times from Matroska.** A track of a Matroska file that starts later or earlier than the picture kept that start only in the listing, as its Stream delay, which the window writes as timeshift=. A meta file without timeshift= muxed every track from the same moment: audio that starts half a second after the picture came half a second early, and TrueHD that starts a few ms after it came that much early. The start is now applied also without timeshift=, and to a join of Matroska files.

* **The sound of a TS, M2TS or Blu-ray playlist lost its start against the picture when the meta file had no timeshift=.** Only the window wrote the start, the listed Stream delay, into the meta. A meta written by hand muxed the sound from the same moment as the picture: sound that starts half a second after the picture came half a second early, and disc sound that starts some ms before the picture came that much late. The start is now taken from the file, as for MP4 and Matroska. A timeshift= still wins. A demux now writes such a start into the name of the sound file (DELAY), so the demuxed files muxed again keep it.

* **The window put the sound of a cut TS too early** when the cut begins with B pictures that are shown before its first picture, by their length, 40 to 100 ms. The listed Stream delay is now measured from the picture shown first.

* **In a join of TS, M2TS or Matroska files ("a.ts"+"b.ts"), the sound of each later file followed on from the sound of the file before,** not from its own start against its picture: a file whose sound starts after its picture, or ends before it, moved the sound of every later file, by seconds with some files. Each file's sound now starts against its own picture. In a join of Matroska files this holds for AC-3, E-AC-3, DTS, TrueHD, LPCM, MPEG audio, AAC and Opus; Vorbis and FLAC still follow on from the file before.

* **With --maxbitrate, video whose own peaks are above the cap reached the decoder after it was due,** even with the average well under the cap: the data was held back at a peak, and the quiet part before it was never used to send it ahead. It is now sent ahead, never more than --vbv-len earlier, so that it arrives in time. The packets, their order and the size of the output stay the same. In the disc cuts measured, 12 to 55 pictures that came late came in time.

* **In a join of subtitle files (.srt, .sup), and of MP4 files with text subtitles, the subtitles of every later file came at their own times,** among those of the first file. Each file's subtitles now start where its video file starts, when the video is joined from as many MP4, MOV or Matroska files.

* **TrueHD with its AC-3 core, muxed as plain TrueHD, lost most of its audio.** A demuxed .ac3+thd file renamed to .thd and muxed as A_MLP had every core frame taken for a damaged frame, and the mux still said it was complete. The core frames are now left out, as drop-ac3-core does, and all of the TrueHD is kept. A_AC3 keeps both, as before.

* **TrueHD that does not begin with a full sync frame played early,** as in a Matroska file after a cut. The frames before the first full sync frame cannot be played and are left out, and the rest of the audio moved forward by their length: 97 ms in a measured file. The audio now starts within a few milliseconds of its time, also when it is muxed with its AC-3 core (A_MLP).

* **H.264 whose start was encoded again by a cutting tool could break up from the cut on.** When SEI and VUI data were inserted (the window's default, "Insert SEI and VUI data if absent"), the first SPS was written for the whole file. The SPS written now follows each change in the stream.

* **The sound of an MP4 came late.** An audio encoder puts start-up samples in front of the sound, and the MP4 says in its edit list to skip them. Only a Matroska output of AAC did. Everywhere else the sound came late against the picture: 21 ms for AAC at 48 kHz, 5 ms for AC-3, 23 ms for MP3. It now starts where the edit list says. A file without an edit list, such as a fragmented file, says it with a longer first sample, and that is read now as well.

* **In a join of MP4 files, the AAC sound fell further behind the picture at every joint.** It now stays within half an AAC frame of the picture, however many files are joined, also for files cut without re-encoding, fragmented files and files without an edit list. A file whose sound is shorter than its picture now leaves a gap in the sound, and the sound of the files after it stays with their picture.

* **The sound of an MP4 cut without re-encoding was out of step with its picture.** Such a file keeps the pictures from the key picture before the cut, and its edit lists start every track at the cut. In a TS the sound came 82 to 109 ms late, and AAC in Matroska 0.5 s early. The sound now comes with its picture, also in a join of such files.

* **Video that names no frame rate of its own was muxed at 25 fps from a TS, an M2TS or a Blu-ray playlist,** as HEVC, H.264, VVC or AV1 may be encoded: a 23.976 film played 4 % too fast and ended before its sound. Without fps= on the track, the rate now comes from the times of its pictures in the file, as it does from Matroska and MP4, also over the next files of a join or the next clips of a playlist when the first is short. This holds when every PES starts with a time, each PES holds one picture, the times keep one step and that step is a standard frame rate; otherwise the track still gets 25, and so does interlaced H.264, whose times may be those of its fields. The listing shows the rate, and the window writes it. Where the listing finds no rate, the window writes no fps= and leaves the rate to tsMuxeR, as the command line does; it wrote 23.976 for every such track. The HDMV descriptor of HEVC, VVC and H.264 now gives the rate the track is muxed at; without fps= it gave 0. Mux such files again. (#46, thanks to @Moonghost-coder for the detailed report)

* **An MP4 whose last box has 0 in its size field was muxed only in part,** and since 2.19.4 the mux still said it was complete. Such a file is now read to its end. A complete mux of an MP4 with boxes after its media that hold no sample (free, skip or uuid boxes, an mdat or moof that nothing uses, a box that says it is longer than the file) said "The output is incomplete"; it now ends as a complete mux, with a warning for an unused mdat or moof and for a box longer than the file. (#42 and #43, thanks to @Moonghost-coder for the detailed reports)

* **An ISO image of a folder with more than 4093 files and folders had its first file damaged,** with --bdmv-to-iso and --keep-extra-files: the table that maps the files was written over the start of it. The table now has the room it needs.

* **A job that was refused could empty an existing ISO image of the same name.** A cut whose end is not after its start, --keep-editions and a FLAC track are refused for a disc, but only after the image had been opened for writing, and --keep-editions on a demux left an empty folder. They are now refused before any output exists; --cut-end=0s still means no end, so it is not refused. Some faults can only be found during the mux, such as a cut that starts after the end of the sources: when the output exists already, the new one is now written under a temporary name beside it and replaces the old one only when the job has succeeded. A job that fails leaves an existing TS, M2TS, Matroska file, ISO image, disc folder or demux folder as it was; a refused demux emptied the files of an earlier demux in its folder. While such a job runs, the drive needs room for both.

* **A subtitle with a long fade could end the mux with a crash,** for example at 59.94 fps. The steps of a fade are written into a buffer of fixed size, and a fade of minutes on a long subtitle ran past its end. A fade that does not fit is now shortened, with a warning.

* **H.264 with pulldown and B pictures got times that were out of order or used twice,** and a Matroska output joined pictures that had the same time: with frame doubling, 60 of 144 times were used twice. Each picture now comes when the pictures before it in the order shown have been shown for their own time. Pulldown that begins later in the stream gets such times from the next IDR picture on, after a gap of a few frames there. The first decoding times of such a stream no longer repeat.

* **H.264 with a deep B pyramid could be decoded after it was due,** in a transport stream and on a disc, as with the 16 B pictures in a row Quick Sync encodes. The decoding times were moved back by 3 pictures at most, and such a stream needs 5. They now move back as far as the deepest pyramid seen so far needs, up to the 16 pictures a decoder holds. Where the pyramid becomes deeper later in the stream, as in a join of files coded otherwise, the decoding times now keep rising there as well.

* **A cut stream that does not begin with an IDR picture could get two pictures with one time,** and a Matroska output wrote the two as one block: H.264 that begins with an I picture of an open GOP, a P or a B picture, H.264 and HEVC that begin inside a run of B pictures, and the IDR after such a start. Each picture now gets its own time, in the order the stream gives. H.264 coded as fields is muxed as before. This now holds for each file of a join as well: a later file that begins so comes after the file before it.

* **On the command line, an output that is one of the sources destroyed that source.** The output was opened for writing, which empties it, while the same file was still to be read. Such an output is now refused before anything is written, also when its name is written in another case or with other slashes. This includes the clips a playlist plays, the file of merge-ac3-file, a Blu-ray folder written over the disc it reads, and a source named as a part of a split output when the job splits. A demux of a raw stream into its own folder wrote the stream over itself and said it was complete; it is refused as well.

* **An MP4 whose AAC sound starts with silence could not be muxed.** AAC writes digital silence in frames of 4 bytes, and such frames stopped the mux with "Out of index for AAC track". Every frame is read now.

* **A chunk after the audio of a WAV file was muxed as sound,** such as a LIST, id3 or JUNK chunk: 1 MB of JUNK became 5 s of noise. The header became sound as well in a WAV whose chunk before the audio has no pad byte, in a WAV whose data size is 0, and in a Wave64 file with an 18 byte format chunk. Only the audio is muxed now.

* **An audio frame at the end of a 2 MB read block could be lost** after bytes that are not sound, such as a tag: one AC-3 or MP3 frame, or the first AAC frame of a file. A byte order mark inside a tag could also become a short frame of sound. Every frame is kept now.

* **One damaged FLAC frame in the last 2 MB of a file lost every frame after it.** One changed byte in a file of 30 s kept 30 of its 1292 frames. The frames after it are kept now.

* **A complete mux of an MP4 with unused bytes at the end of its mdat, more than 1 % of the file, ended with exit -2,** saying the mux stopped before the source was finished. It ends as a complete mux now.

* **In a join of MP4 files, AC-3, E-AC-3 and MP3 sound fell further behind the picture at every joint,** by the encoder's delay each time: 16 ms for AC-3 and 32 ms for MP3 in the files measured. They now stay within half a frame of the picture, as AAC does. E-AC-3 from a single MP4 file started 5 ms late as well.

* **A join of Opus files played what each file leaves out,** and the sound of each later file came about 20 ms late. The padding of each file and the start-up samples of the next are left out at the joint now. The start-up samples at the start of the track and the padding at its end, the last file's in a join, are now left out in a transport stream as well as in Matroska.

* **A timeshift sent subtitles late.** With timeshift=5000ms on the subtitles of a Matroska file, more than half of them came about 10 s after they were due. Their times were right, and they are now sent in time, with a negative timeshift as well. A negative timeshift of many seconds on a high bitrate file can still send the first subtitles late.

* **The time between two clock references could still pass 0.1 s,** with video under 10 frames a second or with subtitles alone: 125 ms and 12 s. Packets that carry only the clock fill such gaps now.

* **Chapters on a disc came out of order, or twice at one time,** when a chapter file was not in time order. The chapters of a file and of --custom-chapters are now one list in time order, whichever option comes first. More than 999 chapters within the title, which a disc cannot hold, were written without a word, or stopped the job after the whole mux when there were thousands; they are refused before it now (from a raw stream, whose length is not known before the mux, every chapter given counts).

* **A Blu-ray split past clip 99999 wrote clip names of six digits** that the playlist could not name. Such a split now stops with a message.

* **--cut-start at or past the end of the sources wrote an empty file** and reported success. It ends with an error now.

* **AV1 from Matroska lost every picture** when the last OBU of each block had no size field. Such an OBU is read to the end of its block now.

* **--cut-end lost the end of the other tracks.** The mux ended for every track at the first packet of any track that reached the cut: the sound before the cut that had not been read yet was lost, 272 ms in a measured 10 s cut, and the two layers of a Dolby Vision disc ended with different pictures. Each track now ends at its own first packet past the cut, and the mux ends only when every track has ended.

* **H.264 film with pulldown played too fast with delPulldown,** a quarter too fast at 29.97 and 2.5 times as fast at 720p 59.94. The marks were removed and the rate stayed, as only MPEG-2 got the film rate. H.264 now gets the rate of its pictures shown once each, 24000/1001 or 24 for film, and VC-1 at 29.97 or 30 gets 24000/1001 or 24, when no fps= is given.

* **A Matroska file whose track list follows its clusters was not read at all.** Matroska allows it, and a current ffmpeg writes it there when its codec data did not fit the room it kept. The listing showed no tracks, and a mux wrote an empty file and called it successful. The track list is found where the file says it is.

* **A join of Matroska files lost the whole second file** when the clusters of the first file all had unknown size, or when it ended inside a cluster whose size runs past the end of the file, and the mux said it was successful. Such a cluster now ends where the next part of the file begins. A file with more than 16 clusters of unknown size is no longer refused, and a video track without DefaultDuration in such a file keeps its frame rate; it was muxed at 25 fps.

* **Drawing text subtitles crashed on Linux and macOS,** at the first subtitle that was measured to see whether it fits the picture: no font was loaded yet. There, a track line without font-name= now ends with a message asking for one when its subtitles are drawn; kept as text, they need no font.

* **On Windows, text subtitles without font-name=, or with a font that is not installed, were not drawn at all,** with a warning that blamed their font size, and the mux reported success. They are now drawn with the system's sans serif font, with a warning.

### MP4 and MOV input

* **More codecs.** VVC, AV1, Opus, ALAC, Vorbis, VP8, VP9 and linear PCM are read. MP3, MP2 and DTS in an mp4a track are read as what they are: they used to be taken for AAC and gave an output that decodes to nothing. A VVC track used to be listed as Opus.

* **MPEG-4 Part 2 video, and ProRes from MOV,** are copied picture for picture into a Matroska output, with the times of the source. Like ALAC, Vorbis, VP8 and VP9, they go into a Matroska output only.

* **WebVTT subtitles** in an MP4 file are read, as SRT text with italic, bold and underline.

* **What the file says about itself** goes into a Matroska output: the track names, the title, the chapters (Nero chapters, or a QuickTime chapter track), the cover picture as an attachment, and the colour, HDR mastering and light level data and the pixel shape of the video, where the video stream does not say them itself.

* **Start times.** Each track of an MP4 starts where the file says: after an empty edit at the start of its edit list, at the start a fragmented file gives it, when its first picture is shown for a video track without an edit list, and after a long first sample, which is how a file without an edit list writes a later start. The sound of such files came early or late, by up to half a second in the files measured. The listing shows the start as the Stream delay, and a meta without timeshift= gets it as well. Text subtitles after an empty edit keep their time too: they came that much early.

* **Joins of MP4 files** keep their sound with the picture, also for cut and fragmented files and files without an edit list (see above).

* **Unusual files** are read: media in more than one mdat box, a MOV with a compressed movie header, a fragmented file with MPEG-4 Part 2 or ProRes, and an H.264 configuration that ends before its picture parameter sets, where the stream carries them.

* **Damaged files.** An audio track with bytes between its chunks made tsMuxeR run without end; those bytes are now left out with a warning. An empty NAL in a video sample, or damage in one picture, used to lose the pictures after it in the same chunk; only the damaged pictures are lost now. A file that was cut off and filled up with zeros gets a warning. An MP4 that was cut off, with its moov at the start, is muxed as far as it goes, and the job then ends with an error that says the output is incomplete: it used to report success.

* **A MOV track with no language** gets none, instead of three control characters.

* **Video with one long still picture** keeps the rate most of its pictures have: one picture held for 2 s in a 25 fps file retimed every picture to 20.83 fps. Video is timed at one rate, so a warning now says when the pictures of a track have different lengths, longer or shorter, also in a fragmented file, and how far from the file's times the video ends.

* **More than 255 chapters** are kept: the Nero chapter list holds at most 255, and a file's chapter track is now taken when it holds more.

* **A fragmented file whose track is not track 1,** as an audio file whose only track is track 2, is read: it could not even be listed.

* **A fragment of a fragmented file with a second run of samples** is read whole: the first run was read twice and the second was lost.

* **Text subtitles of a fragmented file** are read: the file could not even be listed ("invalid stts index").

* **Timed text in UTF-16** is read as such: it came out as broken characters.

* **An edit list that plays a track in parts,** as 0 to 5 s and then 10 to 15 s, gets a warning: the output holds the whole track from where the first part starts.

### Matroska input

* **A join of Matroska files** takes the language, the flags, the colour, the display size and the eye of a 3D base view of each track from its first file: a joined German track came out without a language, and a joined 3D file whose base view is the right eye had its eyes swapped.

* **A file cut off inside a block** is muxed as far as it goes, and the job then ends with an error that says the source is cut off: it was refused whole, with every frame before the cut.

* **AAC from a Matroska file** keeps MPEG-4 in its ADTS headers: they said MPEG-2.

### Subtitles

* **ASS and SSA subtitles are drawn with libass** wherever they are drawn into pictures: with their styles, colours, positions, rotations, fades, moves and karaoke. libass uses the fonts attached to the Matroska file, a fonts folder given with the new track parameter fonts-dir, and the fonts of the system. The Windows packages include libass and the libraries it needs, with their licenses in THIRD-PARTY-NOTICES.txt and the licenses folder. On Linux and Mac, install the system's libass. Without libass, or with the new parameter ass-as-srt, they are drawn as plain text, as before.

* **save-srt** writes subtitles that are drawn into pictures to an .srt file as well.

* **A demux writes ASS, SSA and WebVTT in their own form,** as .ass, .ssa and .vtt files, when they are kept as text. They used to become SRT. WebVTT from an MP4 file still becomes SRT.

* **MP4 text subtitles (tx3g)** keep their times: each was rounded down to the millisecond and the errors added up, so in a file measured the 1200th subtitle came 479 ms early. Their bold, italic and underline go on the right letters, and text outside ASCII is no longer broken by them.

* **An SRT timing line with a tab after the arrow** keeps its end time. The subtitle stayed on screen until the next one.

* **A word of a title after a space or a hyphen is no longer read as a language.** "Pebble Man.ac3" was Mandingo and "Clockwork-Men.srt" Mende. After a space or a hyphen, a code that is also an English word, such as man, men or war, is read only in brackets ([man]); other codes are read there as before ("Movie - eng.ac3"), and after "_" or "." every code is (Movie_spa.ac3, movie.eng.srt). A name with both a language and "forced" (movie_eng_forced.srt, "Movie (forced) [eng].srt") gives both.

* **SRT:** a number alone after an empty line in a subtitle, as a year, is kept as text, also in the last subtitle of a file and in SRT from a Matroska file; it was lost without a word. A block whose timing line has a mistyped arrow is skipped with a warning; it was drawn inside the subtitle before it.

* **Font tags:** colours in single quotes, #RGB and an upper case </FONT> are read. #f00 was drawn dark green.

* **A .sup with bytes in front of its first segment** is muxed whole, with a warning, and is listed. With bytes that begin with "PG", nothing was muxed.

* **Subtitles drawn for video up to 720 pixels wide at 25 or 50 frames per second, such as 720x576, were drawn for 720x480.** The canvas of their PGS was the size of NTSC video, and each subtitle was placed for 480 lines. They now get the canvas of 720x576 a disc has for such video, whether they come from SRT, ASS, SSA or WebVTT. A PGS track already drawn at 720x576 is no longer scaled down to 720x480 when its line names the size and the frame rate of the video. (#45, thanks to @Moonghost-coder for the detailed report)

* **A PGS track scaled to the video size on its line without fps=** had a frame rate of 0 in its pictures. It now keeps the frame rate of the stream.

* **WebVTT captions and descriptions in a WebM or Matroska file** are read, as subtitles. Of the WebM forms, only the kind SUBTITLES was. WebVTT metadata is not text to show and is still not read.

* **The track list names ASS, SSA and WebVTT subtitles from a Matroska file as what they are.** Each was listed as "SRT text subtitles".

* **Drawn text keeps its colour where it is half transparent:** font-color=0x80ffffff was drawn grey, and every soft edge of a subtitle was darker than its colour. On Linux and macOS the alpha of font-color was not drawn at all, and on Windows a half-transparent colour came out more opaque than asked.

* **Text without a border (font-border=0) has no dark outline on Windows.**

* **On Linux and macOS a font that a <font face=...> tag names and the system lacks** is replaced by the track's font, with a warning: the job ended.

* **An SRT subtitle that starts where the one before ends** replaces it on that frame: a clear and the next subtitle came at one time, and the decoding times went back. **A subtitle that ends where it starts, or before,** is left out with a warning: it was drawn with its clear at or before its start.

* **An ASS override block in an SRT file,** such as {\an8}, is no longer drawn as text. The subtitle stays at the bottom; the text kept in a Matroska output or a demux keeps the block.

* **An apostrophe inside a quoted font name** (face="Tom's Hand") no longer hides the color= after it.

* **font-color=** reads #RRGGBB and #AARRGGBB. A negative fadein-time or fadeout-time, a video-width or video-height that is not a size and a font-color that is not a colour are refused, with the value: they made the subtitle invisible, drew it at a wrong size, or drew it black. A fade longer than its subtitle is fitted into it.

* **The palette of a subtitle drawn for HD video is in BT.709,** as its video is: colours were a little off.

* **PGS:** a stream above 1920x1080 is scaled down to the video, and one of 480 lines up for 576-line video. A stream that mixes frame rates gets fps= in every composition. PGS from a Matroska file gets the decoding times of a disc in a .sup demux, a transport stream and on a disc; the times the subtitles are shown are unchanged. A scaled PGS picture is drawn only inside the picture: an object reaching past its edge was written past its buffer.

* **SRT:** a file named .srt must read like SRT: audio named .srt was listed as SRT text subtitles. A default= on a subtitle line wins over a file named forced. Subtitles too large for the picture are fitted faster.

* **Clearer messages:** a missing video-width, video-height or fps is named, and so is an SRT file in UTF-16 or UTF-32 without a byte order mark. A number written twice before a timing line gets a warning, and so do subtitles drawn on fewer lines than the video.

* **PGS from a Matroska file is put back on the frames it was shown on:** its times came out up to half a millisecond beside them, and a player could show a subtitle one frame late.

### Audio

* **AAC whose channel layout is given by a program config element,** such as 6.0, 6.1 or 7.1 with front wide speakers, is muxed: from an MP4 or Matroska file it gave an empty output said to be successful, and a raw file of it was not recognised.

* **A WAV whose valid bits are fewer than its sample size,** such as 18 bits in 24, is read at its sample size: it gave noise of the wrong length.

* **An E-AC-3 stream of more than one program** keeps the time of the first: a stream of two programs was muxed at twice its length.

* **An AC-3 stream that changes its channel layout,** as a join of 2.0 and 5.1, says so; its description keeps the first layout.

* **A raw AC-3, E-AC-3 or DTS file of ten frames or fewer** is recognised, from three frames on.

* **TrueHD at a rate a disc does not have,** such as 44.1 kHz, no longer has 48 kHz in its PMT.

### Video

* **AV1 in an IVF file** is read, and an IVF file read to its end counts as read: a small one ended with "The mux stopped before the source was finished".

* **MPEG-2 whose sequence header gives no frame rate** is muxed whole, at the rate of its picture times, fps= or 25: it went into one PES. The PMT gives the frame rate an MPEG-2 stream is muxed at, also with fps= or delPulldown.

* **What tsMuxeR inserts into H.264, HEVC and VVC** (delimiters, parameter sets, SEI) gets a start code of four bytes, as the format asks at the head of an access unit. It took three where the stream used three.

* **Film at 59.94 with delPulldown** gets no false "irregular pulldown marks" warnings.

### Blu-ray and transport stream

* **hdr10-override** marks an SDR HEVC track as HDR10 on a UHD disc. In the window: "Mark as HDR10 on a UHD disc" on the HEVC track. (#6)

* **SDR HEVC video with BT.2020 colours** is marked as BT.2020 in the playlist and the clip information, as on a UHD disc mastered in SDR.

* **MPEG-2 video** declares its real picture format in the PMT: a 1080i or 1080p stream was declared 720p. With --no-hdmv-descriptors it gets the video stream descriptor of the transport stream standard in a transport stream; a disc keeps its HDMV descriptors.

* **--no-hdmv-descriptors took the HDMV registration out of a disc's program map table,** although the help says the option is not used for a Blu-ray or AVCHD disc. A disc keeps its HDMV descriptors now.

* **Audio at a sample rate a disc cannot carry,** such as AC-3 at 44.1 kHz, gets a warning. A Blu-ray disc carries audio at 48, 96 or 192 kHz only.

* **HEVC level=** reaches the HEVC descriptor of a transport stream made with --no-hdmv-descriptors as well: the descriptor kept the level of the source.

* **Single layer Dolby Vision on a disc** gets a warning that it plays without Dolby Vision, as its HDR10, SDR or HLG base: a UHD Blu-ray player takes Dolby Vision from two layers only. The disc is written as before.

* **The layer break guard goes at the breaks of the disc** given with --disc-size, or --disc-capacity for --bdmv-to-iso: on a 100 GB disc it went at half a 50 GB disc, inside the first layer. The breaks are those of BD-R DL, BD-RE DL, BD-R XL 100 GB and 128 GB, and of the first two layers of a 100 GB disc; a --disc-size close to one of them, such as 50g, gets its breaks with a warning, and another size is refused when a guard is asked for. --layer-break-guard-before on its own gives a guard before the break; it was ignored.

* **A layer break guard of up to 9999 MB** is read from a meta file and by --bdmv-to-iso, as the window offers it. A meta file refused more than 1024.

* **A Dolby Vision enhancement layer listed before its base layer** stays the enhancement layer. It took the PID of the primary video, 0x1011, and the 4K base the PID after it. The base is now told by its size: an enhancement layer is never larger than its base. Each track of the disc is then the same as with the base listed first, also when the base still carries its own Dolby Vision data, and when both layers are split from one track with subTrack=. A Dolby Vision picture larger than the video listed before it, or a single layer one (profile 8.1) of the same size, is no longer taken for that video's enhancement layer, on a disc or in a Matroska file, where it was folded into that video's track and most or all of its pictures were lost.

* **subTrack= on HEVC without an enhancement layer is refused,** however short the track, before anything is written: single layer Dolby Vision or plain HEVC has nothing to split off. The split wrote an empty second track, and every RPU was moved out of the base, so a profile 8 disc lost its Dolby Vision.

* **E-AC-3 without an AC-3 core** gets a warning on a disc: a Blu-ray player has to decode AC-3 but not E-AC-3, so a player without an E-AC-3 decoder plays no sound from it.

* **More than 32 tracks of one kind on a disc** are refused before it is written, with the number to leave out. A 33rd audio track took PID 0x1120, outside the audio range. A transport stream whose tracks need a program map table longer than the 1021 bytes a transport stream allows is refused as well.

* **--maxbitrate no longer replaces the rate of --bitrate.** A constant bitrate disc with the read rate limit, as the window writes it, came out slightly larger. With --maxbitrate at or above --bitrate it is now the same as without it.

* **The finished disc is measured against --disc-size,** as the window says: a warning when the image is larger than the disc, or, for a disc folder, the image its BDMV and CERTIFICATE folders make. Only an estimate was checked, before the mux.

* **On Linux and macOS, a transport stream whose end filled the write buffer exactly was reported as failed,** with "Failed to write the end of the output", although the file was complete.

* **Audio or video that begins after the first 64 MiB of a transport stream** is recognised: it was listed as "Can't detect stream type".

* **A program map that changes inside a transport stream,** at a change of programme in a recording, is said: the new tracks were left out without a word.

* **Two chapter times on one tick of a disc** give one mark.

* **--disc-capacity** is read whole, or refused. A layer break past the end of any disc is refused.

* **An ISO image:** a label longer than an image keeps is said, two names that an image would show as one are refused before the image is written, and an image a refused job made is removed.

* **--minbitrate** no longer lowers the rate of --bitrate, and a --maxbitrate below --bitrate is said.

* **Subtitles after the wrap of a transport stream's clock** are kept: a recording whose 33 bit times go round to 0 lost every subtitle after that point.

* **A disc whose video goes more than 10 s without a key picture** gets a warning: a player starts and seeks at key pictures only, and a stream made with intra refresh has one at its start only.

### Matroska output

* **The display size follows the pixel shape** the H.264 or HEVC video gives. Square pixels at 720x576 or 720x480 were shown at 4:3, and HEVC at 1440x1080 with 4:3 pixels was shown at 4:3 instead of 16:9. A shape within 3 % of a common one, such as 4:3, is written as that one, and when the pixel shape changes within the stream, one shape is used for the whole track. Video that gives no pixel shape is shown as before.

* **Chapters:** a list edited in the window keeps the names of the source's chapters at the same times, and a list emptied in the window gives no chapters. A Matroska XML chapter file gives characters written by their number (&#233;), and a chapter language that is not a language code is written as und, with a warning.

* **A 3D track whose base view is the right eye** is marked so (StereoMode 14), with --right-eye, or from a playlist or a single Matroska file that says so, on a disc as well. It was marked left eye first, which swapped the eyes.

* **20 bit LPCM** says BitDepth 24, as its samples take 3 bytes; it played as noise in other players. A LanguageBCP47 of 40 to 255 characters is kept; a longer one is left out with a warning. The DURATION tags of a source made by ffmpeg are left out, as they give the source's length.

* **A split of sound without video** goes on as long as any of the sound lasts. A subtitle line listed first decided it, and the first sound line did where it was the shorter: the split stopped where that track ended.

* **A cut moves the chapters of --custom-chapters and --chapters-file** as it moves the source's own: the chapter playing at the cut start opens the output at 0. A disc had no mark at its start, and a Matroska output kept the chapters at their old times.

* **The display size a Matroska source gives is kept also when it equals the picture size,** which says that the pixels are square, and also when it is given as an aspect ratio. Such a file over a stream with another pixel shape came out in the stream's shape: 5:4 became 16:9.

* **Every edition of a Matroska file is kept when its default edition shows no chapter.** When all its chapters were hidden, the chapters of every edition were lost. A join of files keeps only the chapters the default editions show.

* **A disabled chapter** (ChapterFlagEnabled 0) of a Matroska source keeps its flag, and one in a chapter file is left out. Both were written as chapters to play.

* **Each video track keeps its own start code framing.** With two video tracks framed differently, one came back from the file framed the other's way and no longer matched its source.

* **--dv-profile=8.1:** a dovi.dll that is there but cannot be loaded is no longer reported as missing. The message names the file and the Windows error. The published dovi.dll needs Windows 8 or newer, so on Windows 7 this option may not work; that is a limit of the library build, not of tsMuxeR.

* **HDR mastering data is written for HEVC only, each track its own:** an SDR H.264 track beside an HDR10 HEVC track was given the HEVC track's mastering display and light levels.

* **ar=** gives the display size of H.264, HEVC, VVC and VC-1, as it did of MPEG-2: it was ignored. H.264 that gives no pixel shape gets no guessed display size: 720x576 with square pixels was shown at 4:3. A DisplayWidth or DisplayHeight of a Matroska source alone is read with the default of the other.

* **A stream that changes its pixel shape** keeps the shape it starts with, and a warning says so: the shape read last was written for the whole track.

* **A StereoMode of a source** that describes the picture (side by side, top and bottom and the others) is kept: such a track came out flat.

* **The default duration of a video track** is rounded: 29.97 fps was written as 33366666 ns, which is 33366666.7.

### Window

* **At its smallest size, every tab stays readable.** The track list keeps at least four rows, the buttons beside it are no longer squashed, and a tab that needs more room scrolls. Rows of the Subtitles and Blu-ray tabs were drawn over each other there.

* **The window shows whether the meta file follows the settings.** Text typed into the Meta file box, or a meta file opened, stops the window from building the meta file from the settings, and nothing said so: a setting changed in a tab was then not in the mux. A box above the meta file, "Build the meta file from the settings", now shows it: it unticks itself when the text is edited by hand, the title of the box says so, a line says when a change in a tab is not applied, and ticking it builds the meta file from the settings again. It takes the place of the "Reset meta to auto-generated" button. (#41, thanks to @ddemarco5 for the suggestion)

* **The German texts** write "Metadatei" one way throughout.

* **The output can no longer be one of the sources.** With the output in the source folder, a source named 00000.M2TS was offered as its own output, as the names were compared with their case, and choosing M2TS or MKV output gave a source's own name (x.m2ts, x.mkv). Such a name now gets "_new". "Start muxing" now also refuses an output that is one of the sources.

* **An output named .mks or .mk3d** is a Matroska output, in the window and on the command line. On the command line it became a folder of demuxed tracks, and the window refused the name.

* **A WebM output can be muxed from the window.** A .webm name chooses MKV output, but "Start muxing" refused it, a click on "MKV muxing" turned it into .mkv, and the Browse dialog had no WebM type and added .mkv to it. A .webm name is now kept and muxed as WebM, and the Browse dialog offers "WebM (*.webm)". (#44, thanks to @Moonghost-coder for the detailed report)

* **New options:** "Draw ASS and SSA subtitles as plain text, without their styles" and "Fonts folder for ASS and SSA subtitles" on the Subtitles tab, "Also save drawn SRT subtitles as .srt files", and "Mark as HDR10 on a UHD disc" on an HEVC track.

* **The Merge AC-3 box follows its track** when rows are moved or removed. It merged another track.

* **An existing MKV or WebM output** is no longer replaced without the question a TS, M2TS or ISO output gets. The parts of an earlier split (name.split.1.ts and on) are asked about too: they were replaced without a question.

* **The level list of an HEVC track holds HEVC's levels.** It offered the H.264 ones. Both lists now hold 5.2, which neither had.

* **"Keep the DTS core"** is offered for DTS-HD High Resolution, which always has a core.

* **"Forced only" on the default subtitle** no longer marks a whole Matroska subtitle track as forced.

* **"Remove all"** builds the meta file from the settings again after it was edited by hand, and --keep-editions is not written for a .webm name, nor for a source that is no Matroska file, a join, a cut, a split or chapters typed in the window: the tick is greyed there. A dropped chapter file is put in time order, and from a Matroska XML file it takes the edition --chapters-file uses, without hidden chapters. The open dialog lists .m2t files. An empty tsMuxerGUI.ini takes over settings that are only under the old company name.

* **The ISO label can no longer hold a double quote.** It ended the label in the meta file, and the options after it were lost.

* **The colour dialog keeps the transparency of a subtitle colour:** 0x80ffffff came back as opaque white, also when nothing was changed.

* **A constant bitrate above the disc's read rate limit** is asked about before the job, with both rates. Only the log said that the stream is held at the limit.

* **When the listing finds no frame rate for the video,** the SRT line gets the 25 the video is muxed at: it got 23.976, and its subtitles were drawn for other pictures.

* **Remove** builds a meta file that was edited by hand from the settings again when it names the removed file: the next mux muxed the removed file.

* **Values read from the settings are checked:** a bad picture in picture corner or size, a blank playlist number of 0 and a subtitle colour that is not a number came back as other values.

* **An output that is a hard link to a source** is refused as one of the sources. The M2TS save dialog offers .m2ts only, and the open dialog lists VC-1, VVC, MP2, MP3 and FLAC files.

### Other

* **Pictures larger than 8 MiB** are read, as in video coded without loss in 4K or 8K, up to 256 MiB. From a Matroska file the mux stopped with "Not enough buffer", and from an MP4 file every such picture was left out. A demux of a picture larger than about 4 MiB wrote past its buffer, and could stop or damage the file.

* **A meta line naming a track a Matroska, MP4 or MOV file does not have** stops the job before anything is written, with the tracks the file has. It gave an empty output that was called successful.

* **A track refused for its output** leaves no empty folders behind.

* **A tab on the MUXOPT line** is read as a space. It joined the texts on either side of it: a tab after --cut-end=5s kept only one picture.

* **Every MUXOPT line counts.** With two of them, the muxers took the options of the last one only. --blu-ray or --avchd on the first of two lines gave a transport stream named .iso, or a folder of demuxed tracks where a disc folder was asked for. Lines that ask for both a Blu-ray and an AVCHD disc are refused.

* **A disc label no longer chooses the disc.** --blu-ray, --blu-ray-v3 and --avchd were looked for in the text of the MUXOPT line, so --label="no --blu-ray here" made a Blu-ray folder, and a label with --blu-ray-v3 in it a UHD disc.

* **A WebM output** no longer carries the BCP 47 languages of its tracks, chapters and tags, nor the track flags that WebM does not have, such as "hearing impaired". A Matroska output keeps them.

* **An ISO image whose folder holds two long names that are both cut to the same name on the image** is refused before it is written, with both names. One of the two files was lost on the image.

* **A damaged frame size in an IVF file** made the frame rate probe read before its data, and the reader kept the rest of the file in memory while it waited for a frame of that size. A size no frame can have now ends the reading there: the frames before it are kept, a warning says how much of the file was left out, and the job then ends with an error, as the output is incomplete.

* **Values that were read as something else are read whole now, or refused with a message:** --cut-start and --cut-end without a unit (--cut-end=5 kept one picture), as a clock time (00:01:30 switched the cut off; it is read now) or with a decimal comma; timeshift= in microseconds (us) or in S (500us was 500 s); fps=24000:1001 (24000 frames a second); --split-size=4G (4 bytes); --split-duration=0 (no split on a transport stream) and 1min (1 s); a level= a codec does not have (level=26 was 4) or that only rounds to one (level=4.15 was 4.2); --layer-break-lbn=12.219.392 (sector 12) and 12,219,392 (three breaks); default=Forced, yes or 1 on a disc (no default subtitle); a chapter time with a comma or a unit, or with minutes or seconds of 60 or more (00:75:00 was 1:15:00); --start-time=10s (10 ticks of the 45 kHz clock); --vbv-len, --extra-iso-space and --auto-chapters, which were read up to the first sign that is not a digit (--vbv-len=0.5s was 0 ms, --extra-iso-space=64K was 64, --auto-chapters=5,5 was 5; --extra-iso-space=-50 ended in an ISO error); --layer-break-lbn=12 219 392 (sector 12); --start-time=99:99:99 (100:40:39); --vbv-len past 2147483647 (held there); --extra-iso-space above 32764 (65536 gave no room, 32765 a refusal after the mux); fps= below 1 (fps=0.001 wrapped the clock); track=1.5 (track 1) and track=-1 (an empty output); an equals sign in a value (--label="Disc=1" was Disc, and a chapter file path was cut there). A second --chapters-file is refused: it replaced the first without a word. A decimal comma on a track line, as in fps=23,976, timeshift=1,5s, 1,500us or 1,5 s (1 ms), is refused with a message, as the values there are parted at commas.

* **An output named .mts or .m2t** is a transport stream. It became a folder of demuxed tracks. --demux writes a folder of demuxed tracks whatever the output is named; with .ts, .m2ts or .mkv it wrote that file.

* **A join of Matroska, MP4 or MOV files naming a track they do not have** stops before anything is written, as a single file does. **--keep-editions with no Matroska source** is refused; it kept nothing.

* **A playlist source** is muxed in the languages its playlist gives, as it is listed.

* **--bdmv-to-iso** counts folders when it makes room for the image's metadata, and takes --extra-iso-space: a disc folder with 400 extra folders was refused after the whole copy.

* **Reads outside the data, and reads at addresses C++ does not allow, are gone.** The search for the start codes of a video stream read up to 15 bytes before and after its buffer, the check for the next TrueHD frame could read two bytes past the data, and many fields of two, four and eight bytes were read and written through pointers the language does not allow at their addresses. A refused job that joined files could read memory already freed while it closed, as its file reader was still opening the next part. None of them was seen to change an output.

* **A playlist is found by every path that names it:** PLAYLIST/00000.mpls from inside BDMV gave no tracks, a playlist in BACKUP could not be muxed, and a name relative to a folder inside PLAYLIST or BACKUP, such as ./00000.mpls, took the clips from the wrong folder.

* **A chapter file with more than one edition** uses the edition a Matroska file would: the one marked as the default, also when the mark follows its chapters, without its hidden and disabled chapters. It used the first edition with them.

* **A meta line that shares some, but not all, of its files with an earlier line for the same track** is refused, with the file it shares, whatever its name or place in either line, as in a join of a.ts and b.ts after a line with a.ts alone, or a.ts alone after the join. Those files were left out without a word.

* **A timeshift or a --start-time so large that the times would wrap round** the 26.5 hour limit of the clock of a transport stream is refused before anything is written. The times went backwards.

* **An AC-3 or DTS frame at the end of a 2 MB read block** was read past the end of the block before more data was asked for.

* **drop-ac3-core on a track that is not TrueHD** says that it applies to TrueHD only and is ignored, and on AC-3 or Dolby Digital Plus that down-to-ac3 keeps the AC-3 core alone. It was ignored without a word.

* **On Linux and macOS, every part of a split Matroska output after the first was damaged:** its size, its index and its SeekHead were written in the wrong places, so it showed no duration. Each part is now whole.

* **An output named .mp4, .mov, .avi or another kind tsMuxeR does not write** is refused: it became a demux folder.

* **Meta lines:** lang= is written in lower case, a quote that is not closed is refused, a source that cannot be opened is named with the reason, track=0 on a Matroska file is named as such, and --keep-editions on a source without editions is said.

* **Every file of a join must have the track, of the same kind:** a transport stream without the PID was left out in silence, and a file whose track of that number is of another kind was read as the codec of the line. The clips of a playlist may lack a track, as a short clip without sound before the film does.

* **A refused demux into a new folder** leaves nothing behind.

* **An Opus, Vorbis, ALAC, MPEG-4 Part 2 or ProRes line over a file of its own,** such as an Ogg Opus file, is refused, as these are read from a container only: it gave noise and a mux said to be successful.

* **On Linux and macOS a source that does not exist got its folders made,** also on a drive that is only read from. Folders are now made only for an output.

### What is still converted or left out

* **MP4 is not written.**

* **WebVTT from an MP4 file** goes into a Matroska output as SRT text, without its cue settings.

* **Floating point PCM** in an MP4 or MOV file is not read, and 32 bit PCM is refused, as from Matroska. Only one form of floating point PCM is refused with a message; the others are left out without a word.

## tsMuxeR 2.20.0

This release is about Matroska. tsMuxeR now reads and writes it much more completely, as RFC 9559 describes it: subtitles in ASS, SSA, WebVTT, DVD (VobSub) and DVB form, ordered editions and linked files, track names and flags, chapters with every title and edition, attachments, tags, splitting, WebM, and new codecs. A remux into a Matroska file keeps each track in the form the source has it. The changes are grouped below by area.

**The Intel macOS build has been back since 2.19.0, and Mac users are still asked to test.** It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. On any Mac, please also try changing the subtitle font and colour in the window, and report what happens.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

### Worth acting on

* **A join of two dual layer Dolby Vision files could pair every picture of the second file with the enhancement layer of its neighbour.** When the enhancement layer of the first file lacks a picture its base layer has, every enhancement picture after the next IDR was placed one picture early. On a disc, and in a Matroska output, each picture of the second file then carried the enhancement layer and the metadata of the next one. The enhancement layer now keeps its place. Mux joins of dual layer Dolby Vision again.

* **A join of Matroska files could lose the end of the last file.** When the files were small enough for the last one to arrive in one read, the track reader took the data of that read for the end of the stream. Two files of 1.26 MB each gave 209 of 254 pictures, and the job stopped with "the output is incomplete". Such joins are now read to the end.

* **A join of Matroska files with Opus audio lost the first packets of every file after the first, and its sound came seconds late.** Two files of 6 s gave a second half that started 3.8 s late, in a Matroska output and in a transport stream alike. Every packet is now kept, with no gap at the joint.

* **In a join of Matroska files, the subtitles of every file after the first were shown at the times of the first file's.** They now follow the video, as the chapters of a join always did.

### Matroska input

* **ASS, SSA and WebVTT subtitles** are read. A Matroska output gets them as they are, with their styles, colours and positions (see Matroska output below). A disc or a transport stream gets them drawn into PGS, with italic, bold and underline.

* **DVD subtitles (VobSub) and DVB subtitles** are read. A Matroska output gets them as they are; a disc or a transport stream gets them turned into PGS, with their colours, positions and timing, and a forced DVD subtitle stays forced.

* **Ordered editions and linked files.** A file whose default edition is ordered is read as a player plays it: its chapters one after the other, and a chapter from another file of the same folder where it says so. The first file of a set of linked files, named on its own, brings the files linked after it.

* **Chapters** are read wherever they are in the file. A disc and a transport stream take the default edition: nested chapters kept, hidden ones left out, every title in every language and the end of each chapter. A Matroska output keeps every edition (see Matroska output below).

* **BCP 47 language tags** (LanguageBCP47) are read. A disc gets the ISO 639-2 code they stand for.

* **WebM and .mk3d files** are read.

* **VP8, VP9, Vorbis and ALAC tracks** are read and muxed into a Matroska output, every frame as the source has it. A VP8 or VP9 track used to be listed as AV1 and written out as an AV1 track that does not decode, and Vorbis and ALAC were not recognised. None of the four has a form in a transport stream or on a Blu-ray disc.

* **A video track without a DefaultDuration**, as many WebM files have, takes its frame rate from the times of its first pictures. It was taken to be 25 and every picture retimed: a 30 fps file of 8 s came out 9.6 s long.

* **An encrypted track** is refused with a message instead of being muxed as noise.

* **VVC and VC-1.** VVC tracks written as the Matroska mapping describes them, and V_VC1 tracks, are read.

* **FLAC.** A FLAC frame is found only where its checksum matches, so no audio is lost to a false frame start.

* **TrackTimestampScale** is applied.

* **A track no reader accepts** is listed with its Matroska codec, so the message says which codec it is.

### MP4 input

* **An MP4 whose fragments come after the first media data** is read to its end.

### Matroska output

* **A remux keeps what the source says about its tracks.** Track names, the forced, hearing impaired, visually impaired, text description, original and commentary flags, the container's colour and mastering data, display size, crop, field order and projection, the file title, the tags about the file and about each track, and attached files are carried to a Matroska output. A remux used to keep only the language and the default flag. Of the tags about a track, the statistics tags are left out (BPS, DURATION and the others named in _STATISTICS_TAGS), because they count the bytes and frames of the source.

* **Subtitles in their own form.** ASS, SSA, WebVTT, DVD (VobSub) and DVB subtitles of a Matroska source are copied into a Matroska output as they are, block for block, with their CodecPrivate, their BlockAdditions and their times. They used to be converted: ASS lost its styles, colours and positions, WebVTT its cue settings, and VobSub and DVB were drawn again as PGS. A text subtitle is copied when it is kept as text (keep-text, the default in the window); without keep-text it is drawn into PGS, as asked.

* **Every edition and every chapter.** A Matroska source with more than one edition, or with hidden or nested chapters, keeps all of them in a Matroska output, with their UIDs, names and flags. A cut and a split move them the same way as the chapters of the default edition. They used to be flattened into one edition, without the hidden chapters.

* **Data stored beside a frame** (BlockAdditions), such as HDR10+ metadata, stays on its frame in a Matroska output, with the track's MaxBlockAdditionID and BlockAdditionMappings. It used to be skipped when the source was read.

* **WebM output.** An output named .webm is written as WebM: DocType webm and the WebM subset of Matroska. It holds VP8, VP9 and AV1 video, Opus and Vorbis audio, and text subtitles as WebVTT. Such a name used to start a demux into a folder of that name.

* **--keep-editions** keeps a Matroska file's chapters exactly as they are, every edition and every link to another file, and its segment identity, so ordered editions and linked files still work after a remux.

* **Splitting.** --split-size, --split-duration and --split-chapters work for a Matroska output. Each part's first picture is at 0, and a keyframe exactly on the length asked for starts the next part.

* **The track flags can be set from the meta:** forced, hearing-impaired, visual-impaired, text-descriptions, original, commentary. --title= sets the file title.

* **A 3D Matroska source keeps its two views in one video track.** Such a remux came out as two video tracks and played as flat 2D.

* **The end of the last frame.** The Duration of a file whose last Opus, Vorbis or ALAC frame is short ends where that frame ends: a file whose last Opus packet was shorter said 1200 ms for 1100 ms.

* **Codec configuration.** A real VVC configuration record, complete H.264 and HEVC configuration records, and the AV1 record with its sequence header.

* **File structure.** Block times to the nearest millisecond, an audio DefaultDuration, BitDepth for FLAC and DTS-HD, FlagInterlaced 2 for progressive MPEG-2, H.264 and VC-1 video, the version in MuxingApp, a CRC-32 in every top-level element, and the SeekHead at the front listing every element.

* **Silence at the ends.** The padding at the end of an Opus track, the priming at the start of AAC from an MP4, and the delay and padding of a LAME MP3 are kept, so none of them plays as sound. The Xing frame at the start of an MP3 is left out.

### What is still converted or left out

* **A WebM output** holds subtitles as WebVTT only. WebVTT stored in the WebM way (D_WEBVTT) is copied as it is. Other WebVTT, ASS and SSA become WebVTT text with italic, bold and underline, without their cue settings, styles, colours or positions. VobSub and DVB subtitles cannot go into a WebM file. A WebM output has no edition or chapter flags.

* **A transport stream and a disc** get every subtitle as PGS and the frames without their BlockAdditions, as before.

* **A demux** writes ASS, SSA and WebVTT subtitles as SRT.

* **The chapters of a join, and of a file played as its ordered edition,** are written as one edition, as before. --keep-editions keeps the chapters of one file exactly as they are, every edition and every link.

* **Tags about an edition, a chapter or an attachment** are left out, as before.

* **A V_VC1 track is written as V_MS/VFW/WVC1.** It is the same stream in the other form Matroska defines for VC-1, so nothing is lost.

### Subtitles

* **A PGS stream rescaled to another video size** was placed below the picture and its colours had too much red. Both are fixed.

### Transport stream input

* **DVB subtitles in a transport stream** are muxed as PGS, as they already are from a Matroska source. They used to be listed as not supported and left out.

### Demux and elementary files

* **VP8, VP9 and Vorbis.** VP8 and VP9 tracks are demuxed into IVF files, and Vorbis tracks into Ogg files. IVF files are read as VP8 and VP9 input, their frame rate taken from the frame times.

* **A demuxed Opus or Vorbis file ends where the source ends.** The samples the source trims from its last packet were played: 13.5 ms for a usual Opus encoder.

### Window

* **MKV output** has a title field and a box to keep the editions and links of a Matroska source. A .webm output name chooses MKV output, and the split options are offered for it.

* **The Matroska page of the track options** has the track name and the six flags, filled from the source and used for an MKV output. The track listing prints a Matroska track's name and the flags it has set.

* **A BCP 47 language of the source is kept in an MKV output.** The window writes lang= for it only when the language in the box was changed.

* **New files.** The open dialog offers .ivf files, and Vorbis, VP8 and VP9 tracks can be demuxed.

### Other

* **lang= in a meta also takes a BCP 47 tag** such as de, pt-BR or zh-Hant. A disc, a transport stream and a demuxed file name get the ISO 639-2 code it stands for, and a Matroska output writes the tag as well.

* **A file named twice in a join** ("a.mkv"+"a.mkv") is read twice.

* **The progress line** no longer shows values below zero.

## tsMuxeR 2.19.4

Two fixes for MP4 and M4A files: a mux that had read the whole file could still end with "The output is incomplete", and a file with its moov after a large mdat could fail at random with "moov atom not found".

**The Intel macOS build has been back since 2.19.0, and Mac users are still asked to test.** It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. On any Mac, please also try changing the subtitle font and colour in the window, and report what happens.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

### Matroska and MP4 input

* **A complete mux from an MP4 with the moov at the end, or from a fragmented MP4, no longer fails.** Since 2.18.16 a mux stops with exit code -2 when it has read less than 99 % of its sources, and the MP4 reader left the moov after the media data and the headers between the fragments out of that count. A short M4A with the moov at the end, or a fragmented MP4, could fail this way although the output was complete. That output is unchanged. A join of fragmented MP4 files without an mfra box stopped after the first file, and now goes on to the next.

* **An MP4 with its moov after a large mdat no longer fails at random with "moov atom not found".** Since 2.19.1 the MP4 reader steps over a box larger than 4 MiB with a seek. A read still under way could land after the seek and mark the file as finished, so the moov was not found and the job stopped. On an 11 MB file with the moov at the end, 9 of 40 runs failed this way. A job that failed this way can be run again.

## tsMuxeR 2.19.3

One fix, for TrueHD merged with its AC-3 core and moved by a timeshift: the audio could reach the player seconds after it was due.

**The Intel macOS build has been back since 2.19.0, and Mac users are still asked to test.** It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. On any Mac, please also try changing the subtitle font and colour in the window, and report what happens.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

### Worth acting on

* **A merged TrueHD track with a timeshift could be sent late, and the sound dropped out.** With merge-ac3-track or merge-ac3-file, whenever the merge waited for more AC-3 data, the muxer took a time for the track that was ahead by up to the size of the timeshift. The audio was then sent up to the size of the timeshift after it was due, and a player ran out of audio for moments. A merge from a Matroska file waits like this often. In a measured mux with a 7 s timeshift, 24,550 audio packets were up to 6.6 s late, and now none are. Without a timeshift the audio stream is unchanged; only its interleave with the video moves. Mux files made with a merge and a timeshift again.

## tsMuxeR 2.19.2

Fixes for a crash in the Matroska output, for the continuity counter of transport streams made from the command line, and for the window, whose settings a tsMuxerGUI.ini beside the program now holds in full.

**The Intel macOS build has been back since 2.19.0, and Mac users are still asked to test.** It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. On any Mac, please also try changing the subtitle font and colour in the window, and report what happens.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

### Worth acting on

* **A transport stream made without --no-pcr-on-video-pid had continuity errors.** Without that option the PCR goes in packets of its own on the video PID, and each of those packets carried the counter of the packet after it instead of the one before. So nearly every PCR gave two continuity errors on the video PID, the sign of lost packets. A TS, M2TS, Blu-ray or AVCHD output without video had the same fault on PID 0. The window puts that option into every meta it makes, so discs and files muxed in the window were not affected, unless the meta was opened from a file or changed in the Meta file box and did not have the option. Mux files made without it again.

### Matroska output

* **A long track name or language no longer crashes the mux.** A track-name= or lang= of 2000 characters ran past the end of a buffer, and the mux stopped and left a file of a few hundred bytes. A long name is written whole now.

### Meta file and command line

* **lang= has to be three letters, as the help says.** Any other value now stops the job, and the message names it. A transport stream kept the first three characters of a longer value without a word, and a demux put the whole value into the file name and failed. lang= with nothing after it still leaves the track without a language.

### Window

* **Cancel in the subtitle colour dialog keeps the colour.** It set the colour to black, so text subtitles drawn into pictures came out black, and the black was kept for the next start.

* **A tsMuxerGUI.ini beside the program holds every setting of the window, and none of them goes to the registry.** The subtitle font, size, style and colour went to the registry, and every start wrote copies of the settings there. An empty tsMuxerGUI.ini now takes over the settings from the registry once, on its first start, the language of the window included. The ini is found in the folder of tsMuxerGUI.exe also when the program is started from another folder. A setting missing from the ini keeps its default instead of being read as 0 or off. To keep the settings beside the program, create an empty file named tsMuxerGUI.ini in the folder of tsMuxerGUI.exe.

* **The window remembers its other choices too:** the bitrate mode and values, the chapter and split choices, the BD-ROM V3 tick, the rate limit, the first MPLS and M2TS numbers, the PIP transparency, the Dolby Vision profile, the dual-layer group, the BDMV folder to ISO tab and the open tab. A choice is saved as soon as it is made. What belongs to one job, such as the cut, the custom chapters, the disc label and the output name, is not kept.

* **Open meta file starts in the folder files are added from**, the same as Browse. It started in the home folder, so on Windows it showed the home folder's view, such as Medium Icons, instead of the view of the folder in use.

## tsMuxeR 2.19.1

Thirty-nine changes, nearly all of them fixes, grouped below by area. Several are for damaged sources, such as a recording that lost part of a packet. A damaged H.264, HEVC or VVC header, or a damaged segment in a .sup file, could stop the job, shift the timing or lose subtitles. Now the mux goes on with a warning. Most of the others are for Matroska, MP4, subtitles, discs and the window.

**The Intel macOS build has been back since 2.19.0, and Mac users are still asked to test.** It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. On any Mac, please also try changing the subtitle font and colour in the window, and report what happens.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

### Worth acting on

* **A 24 or 30 fps MP4 could be muxed as 23.976 or 29.97.** When the video carries no frame rate of its own, the rate comes from the MP4, and a true 24 fps track could come out as 23.976: every picture 0.1 per cent too long, 3.6 seconds an hour behind the sound. Mux such files again.

* **TrueHD longer than 2 hours 31 minutes at 192 kHz lost the rest of its audio, except when merge-ac3-track= or merge-ac3-file= added its core.** The times went wrong from there, and the mux still reported success. At 96 kHz the limit was 5 hours 2 minutes. Mux such files again.

* **level=4.1 on an H.264 track, timeshift=500ns and --cut-start=1.5s did not mean what they say.** level= on an H.264 track dropped its decimal and wrote level 4.0, "ns" was read as seconds, and a fraction in --cut-start or --cut-end was cut off. Files made with such values should be made again.

* **HE-AAC from Matroska or MP4 could be labelled AAC Main in a transport stream, a Matroska file or a demuxed .aac file.** The frames are AAC LC with the HE extension, and a player that trusts the label may refuse them. Make such files again.

* **On Linux and macOS, a full drive did not stop the mux.** A TS or Matroska file was cut short under "Mux successful", and an ISO never finished. The mux now stops with "Error writing file ...: No space left on device (28)". On Windows, a full drive always stopped the mux with an error. A file made on a drive that ran full is shorter than its source: mux it again.

### Damaged sources

* **A damaged HEVC or H.264 header no longer ends the mux.** In HEVC, a parameter set or picture header cut short could stop the job with "Bitstream exception", and in H.264 a parameter set cut short with "Not enough buffer". The output then ended before the damage, or there was none at all. A damaged parameter set is now passed through and the last good one stays in use. A picture with a damaged header is kept, and its time is worked out from the pictures before it. There is one warning of each kind.

* **A damaged H.264 or HEVC picture header could shift the timing of other pictures.** An H.264 header cut short was read on into the next unit and treated as complete, and a picture that lost its first-slice flag was joined to the picture before or given its presentation time. In both cases decoding times were shifted without a warning. On the streams measured, every time now equals the undamaged stream's.

* **A damaged segment in a .sup file no longer loses every subtitle after it.** The reader now finds its way back at the next segment and warns. Before, Matroska output lost the rest of the track, and a mux to a transport stream or a disc could stop with an error.

* **An MP4 box whose size is too small for its contents ends the reading in fewer cases.** Everything after the box was skipped, later tracks included, so tracks were lost or the file could not be read at all. Reading now goes on where the box says it ends, and the first such box is named in a warning. A track whose neighbour lost its tables this way is still read, as long as the file lists the size of each sample of that track and is not fragmented.

* **After a blank line, a mistyped SRT timing line that still starts with a time in the form 00:00:05,000 or 00:00:05:000 no longer puts its block inside the subtitle before it.** A timing line with a wrong arrow, or a colon in place of the comma, made the whole broken block part of the subtitle before it, without a warning. The broken block is now skipped and named in a warning.

### Matroska output

* **DD+ 7.1 said 6 channels.** The track header counted only the 5.1 part. It says 8 now.

* **MPEG-1 video, MLP and DTS Express had the wrong codec label.** They were written as MPEG-2, TrueHD and DTS, so a player that trusts the label may pick the wrong decoder. Each has its own label now, and DTS Express frames are marked so a player can seek in them. Opus tracks now also carry the encoder delay (CodecDelay), which the Matroska rules ask for.

* **A file without video lost a delay given to its audio.** The file started at its first audio frame, so a timeshift on the audio was dropped. A file without video now keeps the source's times.

* **A short video track held a whole Matroska mux in memory.** A second video track of a few pictures, a still for instance, held back the start of the file until the end of the mux. The wait now ends after ten seconds of data. One measured mux peaks at 156 MB instead of 359 MB, with the same output.

### Input files

* **Matroska tracks written with header stripping were read as empty.** Some programs remove the bytes that every frame starts with and store them once in the track header. Those tracks came out empty. They are read now, the same as when every frame is stored whole.

* **An MP4 with its index after the media was read three times.** A 3.4 GB file was read as 10.2 GB to mux, and in full just to list it. It is read once now, with the same output.

* **.mpeg and .m2t files were not read.** A .mpeg file was taken for a bare stream and a .m2t file gave "Can't detect stream type". A .mpeg file is now read as a program stream, and a .m2t file as a transport stream.

### Video

* **VVC pictures are found and timed by the VVC rules.** The type of each unit was read the HEVC way, from the wrong byte, so pictures were split or merged. Of 15 test streams, 11 came out with the wrong number of pictures. In one of them, a stream with two layers, no picture was found at all. An IDR picture with its own order value was shown late, and the pictures after it were shown at the wrong times. On all 15 test streams, every picture of the base layer now comes out once, at its own time. Damage is now handled as for HEVC. On the two test streams that carry their frame rate in the SPS, fps= now writes the new rate there.

* **H.264 with a large SEI unit no longer stops the mux.** With insertSEI, forceSEI or delPulldown, an SEI unit that held a picture timing or buffering period message next to more than 3 KB of other messages could end the mux with "Not enough buffer". Above 4 KB, tsMuxeR could stop with no message at all. Streams that worked before give the same output.

### Subtitles

* **A subtitle too large for the picture is drawn faster.** The search for the largest size that fits measured the same sizes again and again. One measured mux with 150 very long subtitles takes 59 seconds instead of 72, with the same pictures.

* **After a blank line, an SRT counter with a byte order mark is no longer put inside the subtitle before it.** Two SRT files joined byte for byte can leave a byte order mark in front of a counter. Such a counter became part of the subtitle before it, without a warning, and its own subtitle was kept. Now it is passed over like any other counter.

* **The fade times are given in seconds in the help.** The help said milliseconds, so fadein-time=500 as documented gave a fade of 500 seconds. The window always wrote seconds.

### Blu-ray and 3D

* **The clip information named the wrong clock PID.** Without --no-pcr-on-video-pid the clock is on the video PID, or on PID 0 when there is no video, but the clip information said 4097. It names the real one now. The window puts that option into every meta it makes, so discs muxed in the window were not affected, unless the meta was opened from a file or changed in the Meta file box and did not have the option.

* **A playlist or clip file that cannot be written now stops the job, and the error names the file.** A read-only file in the way, for instance, gave "Mux successful" over a disc without it, or with an old copy.

* **Reading a 3D playlist with a text subtitle could put 3D details in the wrong place.** Each entry of the 3D table was looked up as if it were a PG stream, so a text subtitle in front of a PG stream gave its details to that PG stream, and the details of a text subtitle could be written into the memory just before the list of streams. Each entry is now looked up by its place among the PG and text subtitles, and the left-eye and right-eye streams of a stereo subtitle are kept only for a PG stream. tsMuxeR never writes a text subtitle into a playlist, so reading a playlist made by tsMuxeR was not affected. A disc that tsMuxeR made from a 3D playlist with a text subtitle in front of a PG stream could carry the wrong 3D details for the PG streams after it.

* **A 3D movie split into parts kept 3D in its first part only, after BDMV folder to ISO with --3d-single-copy.** From a folder without .ssif files, such as a Blu-ray folder written by tsMuxeR, --3d-single-copy makes them. It made one for the first part alone, so the later parts got none and could not play in 3D, and there was no warning. The window ticks its 3D box, which sets this option, by itself for a 3D folder. Every part gets its .ssif now.

* **Every Blu-ray warning that applies is shown.** One warning hid the others, so with an AAC track there was no warning for a TrueHD track without its core. MPEG audio, Opus, VVC and AV1, which are not Blu-ray formats, are warned about too. The disc itself is unchanged.

### Meta file and command line

* **A misspelt MUXOPT option is no longer lost without a word.** An unknown option was ignored, so a typo changed nothing and nobody was told. It is named in a warning now. A MUXOPT line with only spaces in front lost every option on it without a word, and one with only tabs in front stopped the job with an error such as "Invalid codec format". Both are read as written now.

* **A file named without a folder keeps its name when demuxed.** A file named without a folder in a meta line, such as "tone.wav", was demuxed to a file called only ".wav". The name is kept now.

### Linux, macOS and Windows

* **On Linux and macOS, a font or a disc file behind a symbolic link is found.** Inside a font folder, a symbolic link to a font file or to a folder of fonts was not followed, so a subtitle in such a font stopped the mux with "Can't find a font file". With --bdmv-to-iso, a stream file that is a link was left out of the image without a word.

* **On Windows, output to a 4 KB-sector drive through another drive letter works.** A drive letter made with subst gave no sector size, so Windows refused a TS file written there with "The parameter is incorrect".

### Window

* **The window says why tsMuxeR refused a file, and goes on with the files chosen after it.** When tsMuxeR's check of a file ended with an error, the file was not added, the window did not say why, and the files chosen after it at the same time were not added either. A box now names the file and gives tsMuxeR's own words, and the window goes on with the files after it. A file that tsMuxeR cannot identify, or one that is in the list already, no longer stops the files after it either.

* **A TS or MKV mux no longer becomes a demux over a missing extension.** A name typed without .ts or .mkv came out as a folder of demuxed tracks, and the window reported success. TS output now needs .ts and MKV output .mkv or .mka.

* **A video track that is never clicked follows the window's SEI and SPS choice.** In a file with two video tracks, the one not clicked was muxed with insertSEI and contSPS on, even when they were switched off.

* **Choosing an output before adding a file no longer keeps the name the window filled in.** With the output set to the input file's folder, choosing Blu-ray folder, AVCHD folder or Demux before adding a file set the output folder to the current folder, on Windows usually tsMuxeR's own folder, and adding a file did not change it. Choosing Matroska first kept "default.mkv" in the same way. Both now take the name from the added file. A name made from an added file could also show the folder separator twice in front of the name; it shows one now.

* **The BDMV folder to ISO tab finds a disc whatever the case of its names.** On a drive that tells upper and lower case apart, as is usual on Linux, a disc copy named in other letter cases, such as bdmv or 00001.M2TS, was reported as having no BDMV folder, or as not being 3D. The command line option --bdmv-to-iso already ignored the case.

* **With bibliographic codes chosen, the language box shows them too.** A track tagged "cze" showed "ces (Czech)". It shows "cze (Czech)" now. Only the text in the box changes. The code written is the same as before.

* **The new window texts are translated** into every language of the window.

* **The window keeps its settings under tsMuxeR now.** They were kept under the name of the company the original program came from; on Windows that was the registry key HKEY_CURRENT_USER\Software\Network Optix\tsMuxeR. On the first start, the settings are copied over once, so nothing chosen before is lost. The old place is left as it is, so an older version still finds its own.

## tsMuxeR 2.19.0

Ninety-eight changes: ninety-two fixes and six additions, grouped below by area. Most of them are to Matroska output, to subtitles and to Blu-ray discs.

**Matroska output is still young.** tsMuxeR has only been able to write MKV files since 2.7.2, earlier this year, and a new feature needs time and real use before it is mature. Several of these fixes started with your reports, and looking more closely at the files tsMuxeR writes turned up the rest. Some go back to when MKV output was first added.

More fixes like these will follow, because not everything can be found at once. Thank you for your patience. Every change is tested before it is released, but no test setup has your files, your player and your settings, so something can still behave differently for you.

**If something goes wrong, please report it** with the bug report form on GitHub, and include as much as you can: the version line and the full log, the meta file (the text in the Meta file box of the window), what you expected and what happened instead, and the player you used. A short sample of the source helps most.

**The Intel macOS build is back, and Mac users are asked to test.** Homebrew no longer supports Intel Macs, and the Intel build took its font library from it, so 2.18.15 and 2.18.16 came for Apple Silicon only. The Intel build now makes that library itself. It has been built and checked, but it has not been run on an Intel Mac, because no Mac is available here. If you have an Intel Mac, please try it and report whether it works. If it does not, 2.18.14 is still available and still works. On any Mac, please also try changing the subtitle font and colour in the window, and report what happens: that could not be tested here either.

### Worth acting on

* **A Blu-ray made from H.264 in MP4 or Matroska could not be seeked.** Such a disc had one seek point for each clip, so a player froze or started over when you skipped. Mux it again to get a seek point at every key frame.

* **The blank playlist of --insertBlankPL was empty.** It had no play item, which a strict player may refuse. Discs made with this option should be made again.

### Matroska output

* **LPCM in a Matroska file played as noise in other players.** The track was labelled as plain PCM but held the Blu-ray form of it, with a header on every frame and the bytes in the other order. It holds plain PCM now, and decodes to exactly the source. Blu-ray and transport stream output are unchanged.

* **AAC in a Matroska file could not be extracted.** Each frame still carried the header of the file it came from, which Matroska does not expect. Most players did not mind, but extracting the track gave a file that would not decode at all. The extra header is removed now.

* **VC-1 in a Matroska file could not be opened by other software.** The track described its video with an empty header, so other software dropped the picture or refused the file. The header is written now, and the frames are timed the way this kind of track requires, so every frame decodes.

* **Blu-ray DD+ 7.1 in a Matroska file ran 32 ms late and played as 5.1.** The first frame was given time it does not have, and the track was declared as plain Dolby Digital, so players used the 5.1 core and ignored the extra channels. Both are right now.

* **DD+ 7.1 that is not from a Blu-ray lost its extra channels in Matroska.** The frames that carry channels 7 and 8 were attached to the following frame instead of their own. They stay with their own frame now.

* **The duration of a Matroska file was a little too long.** It added a video frame to whichever track ended last, so a file with TrueHD came out up to 40 ms longer than its tracks really are. Each track is now measured by its own frames. An audio-only file, which came out one frame short, is right too.

* **Subtitle pictures in a Matroska file did not say where playback can start.** The Matroska rules ask for a mark on each block a player can start from, and subtitle blocks never had it. Each subtitle picture that stands on its own carries it now.

* **A player could not seek to a subtitle, or in a file of audio alone.** The index of a Matroska file only listed video key frames. It now lists every subtitle, and in a file without video each audio track about every half second. Each entry also says where in its cluster the frame lies.

* **"No chapters" gave a Matroska output with chapters anyway.** The chapters of the source were taken over whatever was chosen. They are left out now.

* **Two runs of the same Matroska mux could differ in size by a byte.** An identifier was written in a varying length. It always has the same length now.

### Matroska and MP4 input

* **The last block of a Matroska file could be lost.** When a file ended with a block stored together with its duration, which is how subtitles are usually stored, that block was not read. It is read now.

* **Joined Matroska files lost their start code record after the first part.** A Matroska file written by tsMuxeR records how its source framed the video. With "a.mkv"+"b.mkv", every part after the first was given 4-byte start codes, whatever its own record said. Each part keeps its own record now.

* **Fragmented MP4 with more than one track was read from the wrong place.** The second track of each fragment was read from where the first one ended. Every track is read from its own place now.

* **MP4 video could be retimed to an odd frame rate.** Without fps= on the track, a 23.976 film from MP4 could be muxed at 23.9744. The rate given by the file's own timing is used now, rounded to the standard rate it stands for.

* **An MP4 whose movie header comes after its tracks was refused.** The format allows this order, and the start delay of an edit list is now timed once the whole movie box is read, with each file's own header. The entries of an edit list are also read whole now, and no more of them than the box holds.

* **A text track with no time scale stopped an MP4 file.** tsMuxeR ended with a divide error, even when only listing the file. Such a track is now left out with a warning, and the other tracks are read as usual.

* **An MP4 or MOV with a movie time scale of 0 ended tsMuxeR without a word.** It is refused with a message now.

* **A Matroska video block with a broken NAL length could add stray bytes or hang.** A NAL unit is only taken now when its length fits inside its block.

* **MPEG-2 video could be listed and muxed as FLAC.** In some MPEG-2 video, from a Matroska file or a bare .m2v, a few bytes looked like the start of FLAC audio. A FLAC frame now has to pass its own checksum, and a container that names a video codec is asked first.

* **A damaged program map in a transport stream could crash the track scan.** A map that never completed ran past the room kept for it. Such a map is now dropped, and the scan goes on.

### Subtitles

* **Subtitles turned into pictures were split up and could land at the wrong time.** Each subtitle was cut into several pieces, and a piece could be written so far from its neighbours that its time came out wrong: a line due at 6 seconds appeared at 71. Each subtitle is one piece now, written at its own time.

* **Ending a mux early with --cut-end stopped too soon when it had subtitles.** A subtitle ahead of the picture stopped the whole mux, so the end of the video and audio was lost. Subtitles no longer decide where the mux stops.

* **An SRT file could lose its last subtitle, and a large one could lose lines.** The last subtitle was dropped when the file did not end with an empty line. In a file over 2 MB, lines at certain points of the file came out garbled, so a subtitle could show at the wrong time. Both are fixed.

* **A stray line in an SRT file stopped the mux.** A title above the first subtitle, a counter written twice, or a blank line between a counter and its timing line ended the whole mux. Such a line is now skipped, with a warning when it holds text, and every subtitle comes out. A file in which no subtitle can be read at all still stops, and says which line it could not read.

* **An empty line inside an SRT subtitle stopped the mux.** SRT has no way to write an empty line inside a subtitle, but many files contain one. Such a line is now kept as part of the subtitle, and the mux goes on.

* **A line of dialogue with an arrow in it could break a subtitle apart.** Any line holding "-->" was taken for a timing line, so a line such as "Go --> there" after an empty line ended its subtitle and was lost. Only a line with real times around the arrow counts as a timing line now.

* **Some SRT files were read as one long subtitle.** Lines that end with a CR alone, as files from the classic Mac OS do, made the whole file one line. A missing blank line between two subtitles, or a line holding only a tab, joined the next subtitle to the one before. All three are read right now.

* **The encoding of a large SRT file could be judged wrongly.** It is judged from the first 2 MB. A UTF-8 file whose first 2 MB ended inside a letter was read in the code page. A code page file with plain ASCII in its first 2 MB lost its later letters when kept as text. Both are read right now.

* **Subtitles that overlap in an SRT file were drawn out of order.** The end of the first one cleared the second while it was still due, and the pictures could go back in time. Subtitles that overlap are now shown together, the earlier one on top, in time order. At most four are shown at once, with a warning when a file has more.

* **A subtitle could show one frame early.** At 25 and 50 fps a subtitle time can fall exactly between two frames, and it went to the earlier one. It goes to the later frame now.

* **Screen positions after an SRT end time were read as part of the time.** Some SRT files carry positions such as `X1:100 X2:600 Y1:50 Y2:90` after the end time, and the end was then misread. Only the time is read now.

* **Colour tags drew the wrong colour.** `<font color="red">` drew black, and a colour given as #RRGGBB, the form the help shows, was not read at all. Five colour names were misspelt and three had wrong values. Names, #RRGGBB and numbers all work now.

* **On Linux and macOS, red and blue changed places.** A subtitle asked for in red came out blue there, and blue came out red. The colours are kept as they are given now.

* **The words around a style tag were spaced wrongly.** "and <b>both</b>" was drawn as "andboth", and "<b>Bold</b> word" had a gap up to twice as wide as a space. Each piece of a line now moves the next one on by its real width.

* **A subtitle too large for the picture was lost.** It is now drawn smaller, at the largest size that fits, and a warning says so. A subtitle that lies wholly outside the picture is named in a warning.

* **A huge font size could stall the subtitle conversion.** Each step away from the default size was worked out one at a time, billions of times for a large value, and a 19-digit relative size overflowed the sum. Both are held to sizes that can be shown now.

* **Subtitles from some Matroska files came out far too late.** Most files count time in milliseconds, but a file may use a finer step, which the subtitle times ignored: in a file that counts in tenths of a millisecond, a subtitle due at 0.7 seconds came out at 7. Every time uses the file's own step now.

* **Subtitles without an end time in a Matroska file were cleared at a meaningless time.** A subtitle stored without a duration is shown until the next one, as the Matroska rules say, but it was given an end more than two hours later. It is now shown until the next subtitle.

* **A very large PGS subtitle from a Matroska file was lost.** A segment of nearly 64 KB was measured in 16 bits, came out a few bytes long, and was dropped. It is read whole now. Such a subtitle does not fit a transport stream packet, so for TS and Blu-ray output it is refused with a message that names the track.

* **Subtitles from an MP4 file were lost, and the reader could hang.** When several subtitles share one part of the file, only the first was read, at the time of the gap before it, often 0 seconds. Every subtitle comes out at its own time now, with its bold, italic and underline, and a damaged style box can no longer hang the mux.

* **A Matroska file of subtitles alone started at its first subtitle.** Every subtitle came out early by the time of the first one, a delay given to the track did nothing, and chapters no longer matched. Such a file now keeps the source's times, as every other output of the same subtitles does.

* **Times written as text were cut off instead of rounded.** A subtitle from a Matroska or MP4 file could land a frame early, and chapter times in an exported chapter file and times in the track listing could read a millisecond short. Times are rounded to the millisecond now.

### Audio

* **MP3 could play at half or double speed.** Mono MPEG-1 and stereo MPEG-2 Layer 3 were timed wrongly, in every output. MP2 was also declared as MP3 in Matroska, and every MPEG audio track as stereo. All of this is right now.

* **The AC-3 core of a 96 or 192 kHz TrueHD track was sent too early.** When a core was merged into high-resolution TrueHD, its frames went out two or four times too early. They are placed by playing time now, next to the TrueHD unit they belong to.

* **A TrueHD track rebuilt from a Matroska file did not match its source.** When the core of a TrueHD track came from inside a Matroska file, a few core frames were placed one step late. The rebuilt track matches the original byte for byte now. Both halves of the audio were always intact.

* **FLAC with large frames could stop a demux part way.** On the default writer of Windows, a demux of 7.1 FLAC stopped at 0.4 per cent. It runs to the end now.

* **A WAV or W64 demuxed on Linux or macOS had a broken header.** The header kept placeholder sizes, and 8 stray bytes followed the audio. The file is now the same as on Windows.

* **A WAV with a tag chunk began with its own header as sound.** When the header held a chunk of tags of a kilobyte or more, the audio was not found behind it, and the header was played as a burst of noise at the start, in every output. The audio is found behind any such chunk now.

* **A demuxed WAV or W64 could keep placeholder sizes on any system.** This happened when the file came out a whole number of 64 KiB blocks long, so players saw it as broken or empty. Every file now gets its real sizes.

### Video and timing

* **Removing pulldown from MPEG-2 left the frame rate at 29.97.** The help promises 24, but only the window set it; from a meta file or the command line, film then played a quarter too fast. The rate is now set to 24 whenever pulldown is removed and no rate is given.

* **A cut that starts later than the beginning could give no picture.** With --cut-start, the video began on a picture that needs earlier ones, so H.264 and HEVC from Matroska decoded with errors or not at all. The video now starts on its first key frame inside the cut, with everything it needs.

* **HEVC that begins on a CRA picture started 2 seconds late.** The first pictures were timed from the wrong reference. They start on time now.

* **The later parts of a split HEVC or VVC output decoded with errors.** The parameter sets kept for the next part lost their last byte. They are whole now.

* **Joined H.264 parts could get the wrong picture times.** A later part that counts its pictures another way was timed with the first part's step: pictures shared one time, others were spread out, and a warning was printed for every second picture. Each part keeps its own step now.

* **A video that opens partway through a group of pictures started before zero.** Its first few pictures were placed at negative times, and some software could not process the file at all. The file now starts at zero.

* **A Dolby Vision or 3D video could lose its last frames without a word.** This happened when another track in the same mux delivered nothing. All frames are written now, and the usual report is printed.

### Blu-ray, transport streams and 3D

* **The time between two clock references could pass 0.1 s.** The transport stream rules allow at most 0.1 s, and some outputs, UHD among them, went up to 0.11 s. With --maxbitrate it was far worse: when the video asked for more than the cap, the clock references came seconds apart, up to 20 s. The clock is now written in time in both cases.

* **--maxbitrate did not say when the video could not fit under the cap.** Where the video's own peaks are above the cap, its pictures reach the player after they are due, and a player may stutter or freeze there. A warning now says how many pictures come late and by how much, and advises a re-encode with the peak bitrate capped, or a higher cap.

* **The muxer's own tables went out as a burst on Blu-ray.** Every few seconds, four packets left within a few microseconds, far above the rate a drive can read. Such a short burst is now held at that rate.

* **A disc with a default audio track could start with subtitles on.** With an audio track marked default and no default subtitle, the disc's start command could still switch subtitles on. They stay off now.

* **A 3D track gave a UHD disc that plays in 2D.** A UHD disc cannot carry 3D, so a mux with a 3D track is now written as a Blu-ray 3D disc, with a warning.

* **A 3D disc image split into parts was refused.** The base view lost a block of video at every split. Every block is kept now.

* **A 3D playlist could pair a play item with the wrong second view.** Each play item now gets its own dependent-view clip, and one that is left out takes its second view along.

* **A track that starts in a later clip of a playlist was not listed.** Its stream is now found in the clip where it starts, listed and timed.

* **A disc image with many split parts could end without a word.** The room for the file names ran out when the image was finished, after "Mux successful complete". The room is now set aside for every part, and an image that cannot be finished is reported as an error, before any success line.

* **--blankOffset could replace the main playlist or one of its clips.** A number that clashes is now refused.

* **BDMV to ISO made an image from a file it could not read in full.** It stops with an error naming the file now.

* **A 3D folder without its .ssif files gave a 2D image without a word.** With --3d-single-copy, --bdmv-to-iso rebuilds the missing .ssif files; without it, a warning says which titles play in 2D.

* **A file whose end could not be written was reported as complete.** A full disk at the very end of a mux or a demux now fails the run, and the message names the file.

* **The read-rate warning counted the muxer's own tables.** Packets the muxer adds itself are no longer counted against the source's rate.

* **A VVC or HEVC stream could stop a transport stream mux.** Without HDMV descriptors, a VVC stream with many sub-profiles was written past the room for its descriptor, and a stream that was not recognised as VVC or HEVC ended tsMuxeR. Both are handled now.

* **TS, M2TS and Blu-ray output failed on drives with 4 KB sectors.** Windows refused the writes with "The parameter is incorrect". Such a drive is now written through the system cache, with the same bytes; other drives are written as before.

* **A program stream map near its size limit did not fit its room.** The limit for a map in a VOB or MPG now includes its header.

* **The track list showed a negative duration for some broadcast recordings.** The length was read from two different clocks. It is read from one now.

* **Windows file errors were cut to their first letter.** They are printed whole now.

### Chapters

* **Joined playlists and Matroska files lost the chapters of every part after the first.** With "a"+"b", the chapters of the second part were left out, or put at the start. Every part keeps its chapters now, placed where that part begins.

* **A cut kept all the chapters of the film.** With --cut-start or --cut-end, the chapters stayed at their old times. Only the chapters inside the cut are kept now, moved to the cut's own timeline.

* **A Matroska chapter without a name or a language could crash tsMuxeR.** Such chapters are read correctly now.

* **A chapter time of 26.5 hours or more landed near the start.** Such a time is now left out, and a time that cannot be read is refused with its place.

* **A chapter file with a huge chapter number could stall tsMuxeR.** Chapter numbers are checked now, the messages name the right line, and zero-padded numbers are read.

* **A long --auto-chapters step put chapters in the wrong places.** From about 1,580 minutes on, the marks wrapped round, and a very large value never finished the playlist. A step longer than the film now gives one chapter at the start, and a negative value is refused.

### Window

* **The window stopped a mux after 10,000 warning lines.** A mux with many warnings now runs to the end.

* **Lines from tsMuxeR could be cut in two.** A line that arrived in two pieces was shown as two lines. It is shown whole now.

* **The right-click menu of the track list was hard to read in some colour schemes.** It now uses the colours of the system. Selections on Windows 7 also use the Selected Items colours now.

* **The chapter list and the meta line could disagree.** After a join or a removal, the meta line kept the old chapters. Chapter times taken from a source could also read a millisecond early. Both are right now.

* **With Blu-ray ISO selected, the track list could look as if tracks were missing.** The Dual-layer options took their room from the track list, so with a dual layer disc chosen only three of six tracks showed at the window's normal size, and the others were taken for tracks the ISO had dropped. The options sit beside the Meta file box now, and every track shows. The output itself was never affected.

* **A new output name without an extension switched the output to TS.** On Linux, a name typed into the Browse dialog came back without ".iso", and the window took it for a transport stream; a name with a dot of its own did the same everywhere. The dialog now adds the extension of the selected output, and only a known extension changes the output. The dialog also opens in the folder of the output name instead of the home folder.

* **A typed output name could leave the mux set up for the previous output.** Typing a name ending in .mkv while TS was chosen switched the output to Matroska, but the track settings stayed as they were for TS, so a FLAC track was left out of the MKV. Typing .ts or .m2ts while Matroska was chosen kept the FLAC track for an output that cannot carry it. A typed name that changes the output now does exactly what a click on that output does.

* **The window could open in Demux mode with the controls of a mux.** A remembered Demux output came back with "Start muxing", "File name" and a live audio option, until another output was clicked, and a remembered Matroska output also left the audio option live. Changing the language while Demux was chosen put the mux texts back as well. The window now opens as a click on the remembered output leaves it, and a language change keeps the right texts.

* **Bibliographic language codes showed as "und" and could be overwritten.** With the Bibliographic choice of 2.18.15, the language box could not show codes such as "ger", and changing another option on such a track wrote "und" into it, which was then muxed. The box shows the right language now, and a track keeps its code unless you choose another. The arrow keys also no longer stop on a heading of the language list, which left a track with no language at all.

### New

* **An SRT subtitle can stay text in a Matroska file.** Tick "Keep SRT subtitles as text" on the Subtitles tab, which is now on by default, or add `keep-text` to the subtitle line, and each subtitle is copied as text, with its tags and its own start and end, instead of being drawn into pictures. Subtitles that overlap stay as they are. No font or picture settings are needed then. A transport stream or a Blu-ray cannot carry text subtitles, so there the subtitles are drawn as before, with a warning. A demux with `keep-text` writes the subtitles back as an SRT file.

* **The window tells you more about 3D.** The BDMV to ISO tab recognises a 3D folder and ticks the 3D box. The mux tab says when a 3D track gives the disc its .ssif files, and asks before a 3D track is left out.

* **The listing gives an AAC track its bitrate.**

* **--no-chapters leaves the source's chapters out of a Matroska output.**

* **A Matroska file now says which version of tsMuxeR wrote it.** The writing application reads, for example, `tsMuxeR 2.19.0`. Until now a file only said `tsMuxeR`, so the version behind a problem could not be told from the file.

* **Opus in Matroska tells players how much to decode before a seek point.** The Matroska rules for Opus ask for it, and tsMuxeR now writes it.

## tsMuxeR 2.18.16

Sixteen changes, close behind 2.18.15: thirteen faults and three new things.

Most of it comes from one report about Matroska output that turned out to hold six separate faults, and from a question about chapters added to the end of it.

**There is no Intel macOS build this time either.** Homebrew has dropped support for Intel Macs, and the Intel build gets its font library from it, so that half of the macOS job still does not run. Apple Silicon is unaffected and is here as usual. If you are on an Intel Mac, 2.18.14 is still available and still works; a way to build without Homebrew is being looked at.

### Fixed

* **Matroska output timed AVC pictures at half rate.** An H.264 source came out reading 12.000 fps and a few seconds long, with the picture and the sound otherwise intact. Whether picture order counts by one or by two is a property of the stream, and the check that works it out only ever ran when writing a transport stream. It runs for every output now. Transport stream output is unchanged.

* **A mux that stopped before its source was finished still reported success.** The log said the mux had completed even when most of the input had never been read. It now stops with an error saying how far it got. Cutting a mux short on purpose, with a start or an end time, is unaffected.

* **Matroska chapters carried no names.** The writer put out the times and nothing else, so names were lost in every remux and came back as Chapter 01 and so on. Names are written now, each with the language it was written in. This closes the gap mentioned in the demux entry of 2.18.15.

* **The window offered chapters only when the output was a disc.** Matroska holds chapters perfectly well, but the choices were greyed out for it, so the only way to set them was the command line. They are offered for a Matroska output now.

* **The chapter list box could not be edited when the list came from the source.** A disc folder or an MKV filled the box in and then would not let you change it. You can edit it now, and what you leave in it is what gets written.

* **The Strikethrough tick on a text subtitle did nothing.** The window and the muxer spelled the setting differently, so it was written and then ignored. The two agree now, and the tick works.

* **The window wrote a subtitle setting that nothing has ever read.** It sent an alignment value no version of tsMuxeR understands. It is no longer written, and no output changes, because nothing was using it.

* **A default track could not be turned off.** `default=` was read as present or absent, so any value at all meant yes and there was no way to say no. It is read for its value now, and `none`, `no`, `0`, `false` and `off` all mean no.

* **A Matroska to Matroska remux did not keep the default track the source chose.** The flag the source carried was not carried through, so a different track could end up marked. It follows the source now.

* **A subtitle drawn into pictures was declared in the file as text.** When an SRT is rendered to picture subtitles for a Matroska output, the track was still labelled as a text track, which players then read wrongly. A track is declared as what it holds now.

* **The log called a text subtitle PGS.** While drawing an SRT into pictures the progress line named the wrong format. It says what it is really doing now.

* **The help for `--layer-break-lbn` offered a value that could not work.** It printed the default as `12,219,392`, but a comma separates values in that option, so pasting the help's own figure asked for three layer breaks, at sectors 12, 219 and 392, and the guard band was written into the middle of the disc instead of at the layer change. The run still reported success. The help now writes the number without grouping and gives a worked example for each disc size. A sector of zero or less is refused on the command line as well, which it was not before, and a break far too small to be a layer boundary now warns and names the comma as the likely cause.

* **The playlist called a single layer Dolby Vision stream an enhancement layer.** A profile 8 or profile 5 stream was written into the playlist as though a base layer sat beside it, which is true only of profile 7. Single layer discs are described correctly now, and discs without Dolby Vision are unchanged.

### New

* **`--chapters-file` reads a chapter file, and a demux writes one.** It takes the simple text form that eac3to and MKVToolNix produce, and Matroska XML as mkvextract writes it, including a file with a byte order mark. A demux now also writes `chapters.xml`, which keeps the names; `chapters.txt` is still written as before.

* **A chapter file can be dropped on the chapter list box, or pasted into it.** Drop it on the list of times on the Blu-ray tab: the times appear and the custom chapters choice is ticked for you. Pasting `CHAPTER01=` lines works as they stand, with nothing to strip by hand first. A list that came from a file is put in order and has repeats removed.

* **A Dolby Vision stream with no picture for players that cannot read it now says so.** Profile 5 carries no fallback, so equipment without Dolby Vision shows nothing at all. The log warns when it sees one, instead of quietly writing a disc that plays on some players and not on others.

## tsMuxeR 2.18.15

Back at the machine, so this is the first release since 2.18.14. Thank you to everyone who posted while I was away; I have read all of it.

Thirteen changes: six faults, and seven things asked for on the tracker.

**There is no Intel macOS build this time.** Homebrew has dropped support for Intel Macs, and the Intel build gets its font library from it, so that half of the macOS job no longer runs. Apple Silicon is unaffected and is here as usual. If you are on an Intel Mac, 2.18.14 is still available and still works; a way to build without Homebrew is being looked at.

### Fixed

* **A subtitle built from an SRT file came out empty on Windows.** The track appeared in the output and carried nothing, and the mux still reported success. The default subtitle colour was fully transparent, and the Windows renderer honours that, so the text was drawn invisibly and no picture was produced. Naming a colour with `font-color` avoided it, which is why the window was never affected: it always writes one. This reaches meta files written by hand and run from the command line, and it is as old as this fork. The same fault also made a demux of a subtitle on its own write a file of zero bytes. One other way into an empty subtitle track is still open and is being looked at: on Windows, a `font-name` that is not installed also produces an empty track and still reports success, where Linux says it cannot find the font and stops.

* **A status label was hard to read once the window followed a dark theme.** The window takes its colours from the system, so every label here is painted on a light background and a dark one. The message about a read-only disc folder was the one that fell below what it needs on a dark window. No single colour can be right on both grounds, so it is now chosen from the window's own palette: one value on a light window, another on a dark one, each comfortably readable where it is used. The other colours in that part of the window are unchanged.

* **Fonts outside one folder, and every .otf and .ttc, could not be used on Linux.** The search looked in `/usr/share/fonts` only, and only for `.ttf`, so a font you installed for yourself was invisible however you installed it. It now also looks in `/usr/local/share/fonts`, `~/.fonts` and `~/.local/share/fonts`, and it accepts `.otf` and `.ttc`. Naming a font by its full path works for those too. On macOS the user's own font folder was written with a tilde, which nothing here expands, so it had never been searched at all.

* **The Default tracks list on the Blu-ray tab kept showing the language a track had when it was added.** The two entries were built once and never rebuilt, so a subtitle file, which carries no language of its own, went on showing none however often you set one. A track that arrived with a language could go on showing the old one after you changed it, which is the worse half. Nothing about a mux changes: the language you set was always written to the disc correctly, and only the label was out of date.

* **The Browse button offered the wrong file type until you clicked a radio button.** The window remembers the output you last used, but the filter in the save dialog was only set when one of those buttons was clicked, not when the setting was restored. So a window that opened on Matroska or on a disc image still offered to save a `.ts`. The filter is now worked out when the dialog opens, from the button that is actually selected. Reported on the tracker with a patch, and fixed a little differently so there is no second copy of the list to keep in step.

* **The macOS application showed no version.** The window's own title bar had it, but the bundle carried empty version fields, so Finder's Get Info showed a blank where every other application shows a number. It has been that way since before 2.18.14.

### New

* **You can choose which form a language code is shown in.** Twenty languages have two codes in ISO 639-2: Greek is `ell` or `gre`, German `deu` or `ger`. Discs mostly carry the first kind and eac3to reports the second, which is where the request came from. There are three choices rather than two, because wanting a disc's own codes untouched is as reasonable as wanting eac3to's, so there is a choice for that too. On the command line the values are `source`, `terminological` and `bibliographic`; the window shows them as As the source says, Terminological and Bibliographic. Terminological is the default and is what earlier versions did, so nothing changes unless you ask. On the command line it is `--lang-codes=`, which unlike the other options is an argument rather than a MUXOPT entry so that it also applies when listing a file. In the window it is on the General tab and is remembered. A language you type into a file name is left exactly as you typed it. One thing worth knowing if you use the window: because the meta file is built from what the track list shows, the choice there decides what a mux WRITES as well as what you see, so choosing bibliographic authors a disc carrying `ger`, `gre` and `dut`. As the source says gives the most faithful remux of a disc.

* **A demux writes the chapters to a text file.** `chapters.txt` appears beside the tracks, one time per line as `hh:mm:ss.mmm`. That is exactly what the window's chapter box holds, so the file goes straight back in there. On the command line `--custom-chapters` wants the same times separated by semicolons rather than by lines, so they need joining first. It uses the marks in a Blu-ray playlist, and the chapters in a Matroska when they carry a name, which is what most tools write. A source with neither writes no file. One gap worth knowing, and older than this release: tsMuxeR's own Matroska writer does not give chapters names, so muxing to Matroska here and demuxing that file again does not bring the chapters back.

* **`--split-chapters` cuts the output where the chapters are.** Each file begins at a chapter. It uses the same marks as above, and `--custom-chapters` overrides them. As with splitting on size or duration, the cut happens at the first key frame at or after the mark, so it never falls in the middle of a picture. On a disc, whose marks already sit on key frames, the cut lands exactly on the mark. In the window it is a fourth choice beside No split, by duration and by size. Splitting has never been implemented for Matroska output. As with the other two the option is still ignored there and the mux still writes one file, but it now says so instead of passing in silence.

* **A demuxed LPCM track can be written as Wave64.** Add `w64` to the track's line in a meta file and the audio comes out as one `.w64` instead of `.wav`. A WAV header states its sizes in 32 bits, so it cannot describe a file past 4 GiB and a long track had to come out as two, each valid and with nothing lost. Wave64 is the same audio with 64 bit sizes, and tsMuxeR has always been able to read it. In the window it is a tick on the track, shown for an LPCM track when the output is Demux.

* **A meta file can be opened and muxed from the window.** Until now one could only be handed to the command line version. The file is loaded into the Meta file box, and that box is what the muxer is given, so everything in the file is used exactly as written. The track list is not filled in from it and will not match; Reset meta to auto-generated goes back to building the meta from the list.

* **A remove all button.** It empties both lists in one step, and on forty files that is about a tenth of a second against the five or so seconds the same clearance takes one file at a time, because the meta file is rebuilt once rather than after every removal. Removing them one at a time is unchanged and still rebuilds after each one.

* **Tick or untick every track of a type or a language at once.** Right click a track for it. Right clicking an English audio track offers every audio track, or every English track, in one step. A track with no language of its own does not offer the language entries.

## tsMuxeR 2.18.14

As I said when this fork started, it gets attention when I have free time, which comes in bursts and sometimes not at all. One of those quiet stretches begins now: I will have no access to a machine, so this is the last release until I am back. Reports are still worth posting. Nothing goes missing while I am away, and I will read all of them when I return.

Six fixes, five of them found by people reporting something in 2.18.13 and staying with it until it made sense. Two of them are faults in things 2.18.13 itself introduced.

### Fixed

* **A short intro clip was naming the languages of the whole film.** A playlist that opens with a brief clip and then plays the feature has a stream table for each, and they do not have to agree. 2.18.13 began describing a playlist by the streams that most of its running time uses, which is right, but where two clips carried the same tracks it then took the details from the first of them. On a disc whose intro is tagged English and whose feature is tagged Norwegian, a seventeen second intro was naming the language of a one hour forty-nine minute film. The longest clip provides them now. Checked by listing every playlist on eight discs, 2,068 of them, with the old build and the new one: every listing identical, because this only reaches a playlist whose clips carry the same tracks and disagree about the language.

* **The window showed no language for a file that was about to get one.** A raw stream carries its language and its delay in its file name, and both are read when the mux runs. Neither was reported before that, so the track list sat there empty and the only way to find out whether anything had been recognised was to mux the file and look afterwards. Both now appear as soon as the file is added. The delay rule has been in tsMuxeR for years, so this is old rather than new.

* **The read rate ceiling could be written twice, and the wrong one won.** The tick on the Blu-ray tab and Restricted VBR on the General tab both wrote a maximum bitrate, and the muxer takes the last one it reads. A box on another tab could therefore override a tick whose own label promises to hold the disc to the rate a player can read. One ceiling is written now, and the lower of the two wins: the tick is a limit that must not be exceeded, and a smaller figure typed by hand is inside it and is yours to choose.

* **The space bar did not tick or untick a track.** It only ever worked when the highlighted cell was the checkbox itself, and anyone selecting a track clicks its name or its codec, so the key appeared to do nothing at all. It now toggles the highlighted track whichever column has the focus.

* **Nineteen strings had been left in English.** They appeared in the window untranslated in Spanish, French, Russian, Chinese, Japanese and Hebrew, including the whole read rate row added in the last release. Every one of them is translated now, so no language falls back to English any more.

### New

* **A subtitle file whose name says forced is marked as the forced track.** The tools that split the forced subtitles into a file of their own put the word in the name, and setting the same thing by hand before every disc is a step that can be skipped. A Blu-ray has no per track attribute for this: what a disc carries is an instruction at startup saying which subtitle a player selects and whether it shows only the forced parts, and that is what gets set. The word has to be the last one in the name, so `subs (forced).sup` and `movie_forced.sup` are recognised while a film whose title merely begins with the word is not. An explicit `default=` on the meta line always wins.

## tsMuxeR 2.18.13

Three reports arrived on this fork's own tracker while the older list was being worked through,
and all three are answered here. The rest of the release comes from two long standing defects on
the tracker of the original project, [justdan96/tsMuxer](https://github.com/justdan96/tsMuxer).
Neither could be tested before, for want of a file that reaches them. Both files were built here,
and each of them had four more faults standing behind the one it was built for.

### Fixed

* **A playlist was described by its first clip rather than by the film.** A playlist joins clips,
and nothing obliges those clips to carry the same streams. A disc that opens with a short clip of
its own and then plays the feature was described by that opening clip alone: its tracks were the
ones offered, and muxing them read the feature's audio through the wrong reader, which fails with
"AV frame too large" and no buffer size helps. Every play item's stream table is kept now, the
playlist is described by the streams that the greatest part of its running time uses, and a clip
that declares a different codec on a shared PID is left out and named. Checked against 228 real
playlists: no clip left out of any of them, and all 228 listings byte identical to before.

* **A track's language was taken from the clip and not from the playlist.** A Blu-ray writes the
language of a stream in two places, in the playlist and in the clip info file, and a re-authored
disc can have the two disagree. tsMuxeR read only the clip. On such a disc a Norwegian track was
reported as English, where a player and BDInfo both say Norwegian. The playlist wins now, because
that is what the disc's own navigation uses, and a line says where the language came from. A bare
.m2ts opened on its own has no playlist, so the clip is still the only source there.

* **Three of the language code pairs were wrong.** The table that converts the older three letter
codes to the newer ones had Malay and Persian swapped, and pointed Moldavian at Romany. All twenty
official pairs now agree with the list published by the registration authority.

* **The read rate warning told people to re-encode when the muxer was the cause.** It ended by
saying the rate comes from the source and not from muxing, so the only cure was to encode the
video again. Measured on a conformant HD Blu-ray source, which sits exactly on 48 Mbit/s and never
goes past it: remuxed whole, it asks for 118.0 Mbit/s at its fastest, with 22.7 per cent of its
109.5 million packets above the limit. A conformant source comes out over the limit because the
muxer computes arrival times from its own model rather than keeping the pace the disc was
authored at. Capped, the same mux holds 48.0 Mbit/s with not one packet over, every elementary
stream byte identical, for 0.006 per cent of the file size. The warning now names
`--maxbitrate` and the right number for the disc, and keeps the re-encode advice for the case
where the video's own peak really is the problem.

* **A subtitle error named neither the track nor the file.** Someone demuxing thirty two subtitle
tracks got `Invalid SRT format. "" is invalid timing info` and could not tell which of the thirty
two it was. It now names the track, the file and the line number, and says what a timing line
looks like.

* **A subtitle line shorter than two characters walked off the end of it.** The search for the
`-->` counted to `length() - 2` on an unsigned length, so a line of one character or none wrapped
the bound and read past the end of the string. A blank line where a timing line belongs is exactly
that case. On a build where the read reached an unmapped page it came up as "Unknnown exception"
and stood in front of the error the reporter was actually getting.

* **Three bytes off the front of every subtitle file with no byte order mark.** The UTF-8 branch of
the encoding detection skipped three bytes whether it had matched a mark or only found the file to
be legal UTF-8. Usually those three bytes are an index line and one digit of the hour, and the hour
still parses, which is why it went unnoticed. Not always: the same file without its mark put its
first cue an hour early.

* **Memory the subtitle renderer never gave back.** The 8 MB render buffer was allocated with
`new[]` and released with `delete`, in two places. Every glyph rendered took a second glyph for its
outline and never released it. A failing call left a pointer that had already been released, and
the next line wrote through it. All three need a real font on the machine to reach, which is why a
sanitizer sweep on a machine without one found nothing. Measured with the sanitizer, a subtitle mux
went from 24,028 bytes leaked in 105 allocations to none.

* **A meta line that is ignored now says so.** Naming the same file twice, as two tracks, produced
one track and no message, so the output simply had a track missing. The behaviour is unchanged and
deliberate, because a playlist line expands to one file per play item; what was wrong was doing it
in silence.

* **A negative timeshift wrapped the timestamp round.** Asking a track to move earlier by more than
there is start time to absorb sent its timestamp below zero, and a PTS field is 33 bits with no
sign. A track meant to lead the picture by 0.827 s came out more than a day after it, and the mux
reported success. The start of the mux is raised by exactly what is needed instead, so every track
moves together and the offset between them is the one that was asked for.

* **Rejecting bytes that are not this codec cost quadratic time.** When the frame at a false sync
word failed to decode, the reader advanced by one byte from where it had started looking, so the
same false sync was still in front of it and was found and decoded again, once for every byte of
the gap. This is the path a damaged file takes, and the path a wrong codec name on a meta line
takes. 39 MB of TrueHD named as AC-3 went from 11 minutes 16 seconds to 0.9 seconds. The reader
steps past the sync that failed, and the byte accounting, which decides what is reported as lost,
is unchanged on every case measured.

* **The window's language list had a typo, two gaps and eighteen blank rows.** The list is declared
502 long and 484 entries were filled, so the last eighteen rows of the combo box read " ()". `umd`
was a typo for `und`. Blissymbols and "No linguistic content" were missing, the second of which a
real disc carries on a track with no dialogue. And one name had a stray apostrophe, which
translators had been working around. The list is now exactly ISO 639-2, checked entry by entry, and
the declared size and the list that fills it agree so it cannot drift again in silence.

* **The window treated every command line argument as a file name.** `tsMuxerGUI --version` opened
with a dialog complaining that the option is an unsupported format. A real file whose name begins
with a dash is still opened as a file.

* **Some of the window's own strings never translated, whatever language it was set to.** Qt
looks a string up by the class it was written in, and the strings of the read rate row had been
filed under the class that holds the language names, so the lookup never found them and Qt fell
back to the English source. The checkbox, its tooltip and the four rate figures now translate
with the rest of the window.

* **The audio track boxes cut their own labels off.** With a third box added, all three were elided
at the default window size and it had to be made wider to read any of them. They stack in a column
of their own now, which also leaves room for the translations, every one of which is longer.

### New

* **`split-ac3-core`, a TrueHD track demuxed to all three of its forms at once.** A disc TrueHD
track is an AC-3 core interleaved with the lossless part, and a demux could produce any one of its
three forms but only one per run, so getting all three meant three passes over the source. This
writes all three in one pass: the pair as the disc carries them, the lossless part alone, and the
core alone. Checked against three separate runs and against an independent demuxer, byte identical
in every case, and the combined file is the disc's own bytes. It is on the window's audio panel as
well, offered when the output is a demux. It costs twice the disk of the single file, which the
help says.

* **A raw stream's language can come from its file name.** A raw elementary stream has nowhere to
carry a language, so the file name is the only place it can be, and that is where a demux puts it:
tsMuxeR's own writes `...track_4352_eng.ac3`. The delay has been read from the file name in the
same way for a long time. Only the last part of the name is read, only three letters, and only if
they really are an ISO 639-2 code, because tagging a track with the wrong language is worse than
leaving it untagged. So `Movie_spa.ac3` is Spanish and `Space Odyssey.ac3` is not. A bracketed code
is the exception and is read wherever it sits, because eac3to writes one and then goes on
describing the track, as in `... - AC3, [dan], 2.0 channels, 192kbps, 48kHz.ac3`. Nobody brackets
three letters by accident. A `lang=` on the meta line still wins over anything in the name.

* **The window holds a disc to the rate a player can read.** An output that asks to be read faster
than a drive has to supply is not a disc: it plays from a hard disk and may stutter or refuse to
start on a standalone player. Blu-ray folder and Blu-ray ISO output are now paced to the limit by
default, and the label and tooltip say why. The UHD limit is a table rather than a number, so the
tick has a companion choice with the four figures BD-ROM Version 3 gives, defaulting to 109 Mbit/s
because that is what a UHD Blu-ray is mastered to. An HD Blu-ray has one figure and no choice
to make. The command line is deliberately left alone, because `--maxbitrate` has always been
explicit there and scripts must not change behaviour under people.

* **The window says when a playlist clip has been left out.** The command line has always named
such a clip. The window showed one fewer file and said nothing, so there was no explanation for an
output shorter than the disc. It now says so in a box of its own, because a clip is not a track.

* **`drop-ac3-core` is reachable from the window.** It existed only for someone writing a meta file
by hand, though it is the opposite operation to the box that was already there, on the same track.
It is offered only for a TrueHD track that really has a core, and the two boxes clear each other.

* **The window can name a DVB subtitle or teletext track in its own language.** 2.18.12 taught the
command line to say what such a track is instead of printing a bare number. The window rebuilds
that sentence so it can be translated, and could not tell the two apart, because the difference is
in a descriptor it was never given. It is in the line now.

### Changed

* **The Windows packages carry Qt's own translations.** The Yes, No and Cancel of a standard dialog
come from Qt, not from tsMuxeR, and Qt is built here from qtbase alone so that the result runs on
Windows 7, which leaves those files out. They are built from source in both Windows workflows now,
so those buttons follow the language the window is set to.

* **Every published asset gets a SHA-256 beside it.** A release published five files and no way to
tell whether a download arrived intact. Each asset now has a `.sha256` next to it, in the format
`sha256sum -c` and `shasum -c` read.

## tsMuxeR 2.18.12

The issue list of the original project, [justdan96/tsMuxer](https://github.com/justdan96/tsMuxer),
was read through again for this release. This time the feature requests were read with it. Six of
them turned out to be implemented here already. Two of the oldest defects could never be tested
before, for want of a file that reaches them. Both are fixed.

### Fixed

* **Pulldown flags were read and then ignored, so such a file came out short.** A pulldown flag
says a picture is held on screen for longer than one frame. The stream carries no extra picture for
that time. tsMuxeR reported the right frame rate, then laid the pictures out one per frame at it.
Six seconds of film came out as four point eight with 3:2 pulldown, and as three with frame
doubling. All three kinds of flag now give the length the source really has.

* **An audio track could be reported as video, with a resolution.** A track the container declares
as MPEG audio was offered to six video detectors before its own. One accepted it and answered
"AV1, 68x28". A confident wrong answer is worse than no answer. Where the container names the
format, that reader is now asked first. A wrong declaration still falls through to everything
else.

* **`--maxbitrate` did nothing at all, despite its name.** Asking a disc to stay under a rate
produced a byte identical file. It now paces the stream so that it never asks to be read faster
than the rate given, which is what a source with bursts in it otherwise does. A fixed rate is still
`--bitrate`, and that is unchanged.

* **A playlist could not find its own streams.** A disc keeps playlists and clips in separate
folders. tsMuxeR works out where the clips are from where the playlist is. Written with forward
slashes, which Windows accepts everywhere, that came out empty and the clip was looked for in the
wrong place. Written with no folder at all, the same. Both work now.

* **Advice meant for the user was printed twice.** Every file is opened twice, once to see what is
in it and once to mux it, and a pass that only looks at a file no longer talks.

* **A track table was reported as an unknown number when the container had named it.** A DVB
subtitle or teletext track is now named, along with the fact that it is skipped.

* **The VC-1 level printed as a control character.** Level 3 came out as the byte 3, which shows as
whatever the console makes of it. It prints as a number now.

* **The font box on the Subtitles tab was a row too short.** The fourth row, Options, was cut off
and a scroll bar appeared to reach it. All four rows are visible now.

* **Memory that was never released.** Every Matroska file opened left behind what its tracks owned:
the codec private data, the codec id, and the parsed private data object. Every HEVC or VVC unit
built left behind its buffer, and the detection probe builds four of those for any file it looks
at, whatever the codec. Every mp4 with a glbl or an avcC atom left behind its codec private data.
Measured with a sanitizer over eight files of different kinds and five real muxes, all of which are
now clean.

### New

* **A TrueHD track can be demuxed on its own.** A disc TrueHD track carries an AC-3 core
interleaved with its lossless part. A demux has always written both to one file. A plain decoder
reads that file as AC-3 and then fails on the rest. Add `drop-ac3-core` to leave the core out and
get the lossless stream by itself, named `.thd`. Without it the demux is unchanged.

* **A DVB subtitle or teletext track is now named.** Such a track used to be reported as an unknown
number. It is now named from what the container says it is, along with the fact that it is skipped.
Muxing them is still not implemented.

* **The window remembers which output was last chosen.** Someone who always demuxes had to set that
back on every start.

### Changed

* **`timeshift` also shifts video, and the help said audio only.** It has always worked on a video
track. It is now documented in both places.

* **Qt's own buttons can be translated.** The Yes and No of a standard dialog come from Qt, not
from tsMuxeR. The Windows build has nothing to translate them with. The program now looks for
those files beside itself, so they can be added.

* The other seven translations follow the audio option renamed in the last release. They name the
new wording and are marked for translation rather than carrying the old promise forward.

* The build workflows moved off the actions that still run on Node 20, which every run had been
warning about.

### Thanks

Particular thanks to **@Frank-Z7**, who reported an allocation mismatch in
[justdan96/tsMuxer#778](https://github.com/justdan96/tsMuxer/issues/778) with a sanitizer log and
screenshots of the code with the offending line marked. That mismatch had already been repaired,
but the report was complete enough to find the rest of the job that repair had left undone, which
is where most of the memory work in this release came from. A report that good is rare and it
deserves saying.

## tsMuxeR 2.18.11

Every issue on the tracker of the original project, [justdan96/tsMuxer](https://github.com/justdan96/tsMuxer),
has been read through and tested against this version, to see which ones are still there and which
are already gone. This release fixes the ones that are still there. The feature requests have not
been looked at yet. Defects come first.

### Fixed

* **Joining mp4 files kept only the first one.** Four files went in, one came out, and tsMuxeR
reported success and exited zero. Nothing in the output said a quarter of the film was missing.
Joining `.264` or `.mkv` files was never affected. Five separate faults were behind it. One of
them could crash while simply reading an mp4. Another made the same command work on one file and
fail on another of a different length. Joining now gives the whole thing.

* **A file that opened correctly could crash tsMuxeR at random.** It happened about twice in a
hundred runs on one ordinary mp4. The buffer for a frame was measured by one piece of code and
filled by another. Only one of them checked, so a bad length wrote past the end of the buffer.

* **Asking for a different HEVC frame rate did nothing and said it had worked.** The rate was
accepted and announced in the log. It was never written into the video. Every rate now lands in
the file. This also fixes muxing HEVC with a changed frame rate producing an unreadable result.

* **A frame rate written as a fraction was read as its first number.** `fps=24000/1001` was taken
as 24000, so the disc was built at twenty four thousand frames a second. Only a meta file typed by
hand could reach it, which is exactly what a recipe asks people to write.

* **`--cut-end` was described as a trim.** It is not. It is the point to stop at, measured from
the start, so `--cut-end=30s` keeps the first thirty seconds rather than removing the last thirty.
The help now says so. A range that selects nothing used to write an empty file and report success.
It is now refused, with both values named.

* **An empty output path came back as an unknown exception.** A path of nothing but spaces was
accepted outright and muxed as though it were a file name. Both are now refused with a clear
message, and the underlying read past the end of an empty string is gone.

* **A file whose name does not match what is inside it is now refused.** The name is what decides
which reader opens a file. A recording called `.mpg` is very often a transport stream, and it was
read as a program stream. That gave either a message about the reader's own internals, or for
several combinations a banner and nothing else at all, at exit 0. The message now says what the
file really is and what to rename it to. Correctly named files are unaffected.

* **A changed HEVC frame rate reached only half of the file.** HEVC records its timing twice, and
`fps=` rewrote one copy and not the other. The file then disagreed with itself: some tools read 25
and others read the original rate from the same file. Both copies are now written.

* **A source with more than 64 tracks could not be opened.** The limit is now 256.

* **Every mp4 opened leaked memory.** About 13 KB per file, and once more for each file of a join.

### New

* **tsMuxeR now says when a disc asks to be read faster than a player has to manage.** A Blu-ray
drive only has to supply 48 Mbit/s, or 109 for UHD. A video whose average sits well inside that
can still ask for far more in bursts. That happens when its peak rate was never capped at encoding
time. Such a disc plays perfectly from a hard disk. On a standalone player it may stutter or
refuse to start. Nothing in the output changes. The warning names the rate and the limit.

* **A track that produced no frames at all now says so.** The codec named on a meta line was taken
as fact and never checked against the file, so naming the wrong one was silent. `A_AC3` on an H.264
stream wrote nothing and said nothing, and a video codec on an audio file wrote a file of the
expected size holding no frames and reported success. Any track that delivered nothing is now named
at the end of the mux.

* **The window opens where you last left it.** It is put back on a screen that still exists.

### Changed

* **The audio option called "Downconvert" is renamed, because the name can mislead.** tsMuxeR is a
muxer and does not convert audio. TRUE-HD, DTS-HD and DD+ carry a plain core inside the HD stream,
and all this option ever did was keep that core and drop the HD part. It has had that name since before this project was
forked, and it suggests something the program has never done. It now reads "Keep the AC-3 core, drop the TRUE-HD part", and the same for DTS-HD and
DD+. It is also only offered where there is a core to keep. Nothing about what it does has changed,
and `down-to-ac3` and `down-to-dts` in a meta file are unchanged.

* **The help for `--maxbitrate` no longer describes something it does not do.** It said the option
sets the rate the stream is paced to. It does not pace anything, and on its own it changes nothing
at all. Its one real use is to switch on the padding that `--minbitrate` asks for. For a fixed
rate use `--bitrate`.

* **The macOS packaging script no longer depends on where it is run from**, and the install
instructions no longer point at an address that has gone.

## tsMuxeR 2.18.10

The macOS app would not open on Apple Silicon. No arm64 build ever has.

### Fixed

* **On Apple Silicon, macOS reported the app as damaged and refused to open it.** Every arm64
build from 2.8.0 onwards was affected, and the x64 builds under Rosetta were not. It was not a
security prompt that could be clicked past. Two faults in the packaging caused it, both silent:
the build succeeded, the download worked, and the app would not start.

The app bundle was never signed. Nothing wrote a signature for it, and on Apple Silicon that is
what macOS objects to. It now carries an ad-hoc signature.

The frameworks inside it were flattened. Packaging followed the symbolic links instead of storing
them, so `Versions/Current` became a real folder and each Qt library a real file rather than a
link. A framework in that shape is not valid, which is a second route to the same message. The
links are now kept.

The macOS download is also about half the size it was, 42 MB rather than 81 MB for arm64. Nothing
has been left out. Every Qt library was previously stored three times over, once for the real file
and once for each link that had been turned into a copy.

An ad-hoc signature is not a Developer ID. macOS will still say the developer cannot be verified,
which right-click and Open clears. Removing that as well needs notarisation.

Reported by @FRtranslator.

## tsMuxeR 2.18.9

Opening a Matroska file could take minutes, or look as though the program had stopped. That is
fixed, and it had been happening since 2.18.0.

### Fixed

* **Opening a Matroska file could take minutes.** Every mkv that was opened had its whole top
level walked, cluster by cluster, to look for attachments. On a long file that is thousands of
seeks before the track list appears, and a 36.6 GB file holds 7,293 clusters. On a local SSD it
takes under a second, which is why it went unnoticed. On an external or network drive it is
minutes, and it looks like a program that has hung. The file's own index is now read instead,
which answers the same question in 6 element reads rather than 7,302. Files that carry no index
at all still take the old route, and attachments are still found wherever they sit, including
after the clusters.

* **An error box could appear with nothing written in it.** Closing the window while a file was
still being examined produced a box with an icon, an OK button and no message, which did not even
say what had stopped. It now says the muxer ended without giving a reason, and that nothing has
been written.

* **The program reported the wrong version number.** 2.18.8 said 2.18.7. Nothing behaved
differently, but anyone checking which version they were running, or saying so in a report, was
told the wrong one.

Reported by @Shay2242.

## tsMuxeR 2.18.8

A single layer Dolby Vision source built a disc that would not play. That is fixed, along with
four other faults, and an added .m2ts from a 3D disc now says when it is only half of the picture.

### New: an added .m2ts from a 3D disc says it is only half of the picture

A 3D disc gives its video three names: an .m2ts holding the base view, a second .m2ts holding the
dependent view, and an .ssif holding both. Adding one of the .m2ts files looked like ordinary 2D
video, with nothing to say otherwise, and a disc built from it came out flat.

The track information now says the file is the base view only, and names the .ssif that holds both
views, so the right file can be picked without knowing the layout beforehand.

### Fixed

* **A single layer Dolby Vision source built a Blu-ray or an m2ts that would not play.** Profile 5
and profile 8 carry their RPU in the base layer and have no enhancement layer, so such a source has
one video stream. That stream was written to the enhancement layer PID, 0x1015, where no player
looks for a picture, and the disc played as nothing at all. It now goes to 0x1011 with all other
video. Dual layer profile 7 sources were never affected.

* **A warning said `SPS parameters is not consistent throughout a stream` on streams that never
change.** It compared the first parameter set it saw against a value that nothing had set yet, so
it fired because the stream was new rather than because anything in it differed. It still fires
when a stream really does change.

* **The window could not set an HEVC level.** The control was greyed out for an HEVC track, so the
only way to ask for one was to type `level=` into the meta file by hand, which the muxer has always
accepted. The list of levels also stopped at 5.1, and now goes to 6.2.

* **Fitting to a dual layer disc left the layer break unprotected.** Choosing a disc under
"Fit to disc" set the capacity and nothing else, so the size was checked and the break was not. A
disc with 2 or more layers now turns the layer-break guard on with it. It is a default and not a
lock: unticking it afterwards sticks, and only choosing another disc sets it again. Off and the
single layer BD 25 clear it, because they have no layer break for a guard to sit on. The BDMV to
ISO tab already worked this way.

* **A Dolby Vision message said base view where it means base layer.** Base view is a 3D term.
Nothing but the wording changed.

Reported and tested by @Coopervid and @DreckSoft.

## tsMuxeR 2.18.7

A Dolby Vision MKV or MP4 remuxed to MKV now keeps its Dolby Vision. Before this it did not, and
nothing said so.

### Fixed

* **A Dolby Vision MKV or MP4 remuxed to MKV lost its Dolby Vision.** Such a file holds both layers
in one video track. Read back with `subTrack=` and written to Matroska, it gave two plain video
tracks with no Dolby Vision records on them. What went in as Dolby Vision came out as plain HDR.
Muxing the same source to a disc was never affected, and neither was any other output.

* **Nothing found about a subTrack stream reached it.** The cause was wider than Dolby Vision. The
probe looked for such a stream under a number the container never uses, so it was skipped and
everything it would have found was lost, the frame rate included.

A Dolby Vision MKV from this version is read back with `subTrack=1` and `subTrack=2`. One from an
earlier version still wants `track=1` and `track=2`, because it holds two separate tracks rather
than one.

## tsMuxeR 2.18.6

An mp4 read with `subTrack=` could crash the muxer. That is the only change.

### Fixed

* **An mp4 read with `subTrack=` could crash the muxer.** An mp4 whose video track holds a merged
Dolby Vision picture crashed when it was read back with `subTrack=` and no `fps=` on the meta line.
The frame rate lookup was handed a sub-track number and read past the end of its array. The mux
died with an access violation and wrote nothing. The same file in an mkv was never affected,
because the Matroska side of that lookup already checked its range.

Present in 2.18.4 and 2.18.5. Nothing outside that case behaves differently.

## tsMuxeR 2.18.5

A 3D disc muxed to MKV now keeps both views in one video track, so the file is read as 3D
instead of flat. No behaviour outside 3D changes.

### 3D to MKV: both views in one track

A 3D source has two video streams, the base view and the dependent view. Until now each became a
track of its own, and the second one carried a codec id that general software does not recognise.
The file rebuilt a disc correctly, but nothing else could see the second view, so it was read as
flat 2D everywhere.

Both views now travel in one track, which is the form other software writes:

- the dependent view's NAL units follow the base view's in the same block
- the track is marked StereoMode 13, both eyes in one block, left eye first
- the MVC configuration record is written as the "mvcC" block addition mapping, built from both
  views' parameter sets

There is no option to set. A source with no dependent view is muxed exactly as before.

A full length 3D source of 40 GB came out as a 34.1 GiB MKV of 2 h 5 min, read back as two views
at Stereo High, with its TrueHD and AC-3 tracks intact.

### Fixed: the last NAL of an access unit could be dropped

When a combined AVC and MVC track was split back into two views, a NAL unit of exactly four bytes
at the end of a packet was never read, because the loop stopped four bytes short of the end. An
access unit ending with the end of sequence NAL lost it.

This runs for combined AVC and MVC tracks only, so nothing outside 3D can reach it.

### Fixed: the subTrack documentation was the wrong way round

For a combined AVC and MVC track, subTrack=1 is the MVC part and subTrack=2 is the AVC part. The
usage text and the documentation both said the opposite, so a meta file written from them put each
view in the other's place.

They also both said subTrack was for combined AVC and MVC tracks only. That stopped being true when
dual layer Dolby Vision began using it, where 1 is the base layer and 2 the enhancement layer. The
two cases do not number their parts the same way, and the text now says so.

### How it was checked

A disc was built straight from the source, and a second one from the MKV, then compared file by
file:

```
before this release   12 of 12 files identical
after this release    12 of 12 files identical
```

The MKV changed completely in between and the disc did not move at all. The same comparison was
run against a 3D file written by other software, with the old build and the new one: 12 of 12
identical both ways.

## tsMuxeR 2.18.4

A 3D disc can now be wrapped into an ISO without writing its video twice.

### New: `--3d-single-copy`

A 3D Blu-ray stores its video ONCE and gives it three names. The base view and the dependent view
are cut into chunks and written alternately, and that single run of sectors is what the `.ssif`
names. The same sectors are addressed again as two `.m2ts`, one naming only the base chunks and one
only the dependent chunks, so a 2D player opens the base `.m2ts` and never sees the rest.

Copying all three by name writes the video twice. 2.18.3 started reporting that; this writes it
once instead, so all three names resolve to the same sectors again, as on the source disc.

```
tsMuxeR --bdmv-to-iso --3d-single-copy <BDMV_folder> out.iso
```

Both facts it needs are stated on the disc, so nothing is guessed: the clip info file lists where
the chunks divide, and the playlist names which two clips pair up. A group whose five files are not
all present, or whose two views are not cut into the same number of chunks, is left alone and copied
as found.

Off by default. Without it the image is byte for byte what it was before.

Measured on a six-group 3D structure, with 1, 2, 2, 4, 4 and 4 chunk pairs:

```
without   366,084,096 bytes, every chunk of video present TWICE
with      188,088,320 bytes, every chunk present ONCE, dependent view first
```

All 18 files read back byte for byte identical to the source, checked twice with two independent
readers: 7-Zip, and the operating system's own UDF driver, which is what a player uses.

Then on a full-size 3D structure, 852 files and 40.8 GiB, its main title interleaved in 1,774
chunk pairs:

```
image        43,796,791,296 bytes, against 43,800,002,560 for the source it was built from
a plain copy would have written 87.2 GB
files        852 of 852 byte for byte identical, 0 different, 0 missing, 0 added
```

The source stores its video once, and the rebuilt image lands 3.21 MB from the same size, which
is the difference in UDF overhead and nothing else. `0 added` says the source's own `BACKUP` came
through untouched rather than being regenerated.

### The 3D authoring path was verified, not changed

tsMuxeR has authored 3D discs from streams for years, and `createInterleavedFile` is shared between
that path and the new one, so it was run rather than assumed. Muxing the two views of a 3D source
to a Blu-ray ISO gives a base view and a dependent view whose sizes sum to the `.ssif` exactly,
inside an image too small to hold the video twice, with the dependent chunk first. Building a disc
that way and then copying it with `--3d-single-copy` returns every file byte for byte.

Note that only a Blu-ray ISO can hold this. Two files sharing one run of sectors is a property of
the UDF image; a folder on an ordinary filesystem cannot express it.

### Fixed: an interleaved 3D file could be built wrong instead of refused

`createInterleavedFile` guarded its two preconditions with assertions, which a release build
removes. Both mean the interleave cannot be described: unequal piece counts leave chunks with no
partner, and a part-sector piece cannot be addressed by a non-final allocation descriptor. Building
it anyway produces a file of exactly the right length with its two halves out of step, which no size
check catches and no reader reports. Both conditions now say what is wrong and refuse.

The 3D authoring path calls the same function and ignored its result, which was reasonable while
the only outcome was an assertion a release build deletes. It acts on a refusal now: carrying on
past one would end the build with "Mux successful complete" while leaving a zero-length `.ssif`
in the image, which is a 3D disc that cannot play, reported as a success.

### The BDMV folder to ISO tab has a checkbox for it

The option was reachable only from the command line, and that tab is where someone wrapping a 3D
disc actually is. One checkbox next to the other build options, off like the option itself, with a
tooltip explaining the three names. Translated into all eight interface languages.

### Fixed: leaving out the output path printed one character

`--bdmv-to-iso` was only routed to its own handler when at least four arguments came with it, so a
call that forgot the output path fell through to the ordinary muxing path, which tried to open
`--bdmv-to-iso` as a meta file and printed a single character of the resulting exception. The mode
is routed on its own name now, and all three of `--bdmv-to-iso`, one argument and three arguments
print what the mode expects.

### Fixed: the layer-break guard could move a UDF structure and corrupt files

**This is not a 3D fault.** It was found while testing the 3D authoring path, but it reproduces on
an ordinary copy with `--3d-single-copy` off and no 3D content involved.

**Who it reaches.** It needs a disc of roughly 7,000 files or more, with `--layer-break-guard` or
`--inner-only` in use. For scale, a full feature disc measured here holds 852 files, about a ninth
of that, so an ordinary disc was never at risk. If you built an image far larger than that with an
earlier version, a BD-J disc for instance, the result is worth checking. The damage is silent: the
build reports success and the files look normal.

The UDF unique-id mapping file lives at a fixed address just past the metadata partition, and it is
written at the very end of the build by seeking BACK to it. `FileEntryInfo::write` consults the
layer-break guard on every write, so a guard zone that reached back that far padded the mapping file
FORWARD, out of its own home and into the data area.

The guard exists to move file data off the sectors around a layer transition. A volume structure has
nowhere else it may live, so moving one does not protect it, it destroys the image. The guard is now
suppressed while such a structure is written.

The metadata region grows with the file count, so the two eventually meet. Measured on a 12,005 file
tree with `--inner-only` on a BD50, which is a supported combination and not a contrived one:

```
before   the mapping file's home is sector 12,928, the guard's before-zone begins at 8,704,
         so the structure was written at sector 24,429,792 and four navigation files
         (index.bdmv, MovieObject.bdmv and both .clpi) came back wrong
after    12,005 of 12,005 files byte for byte identical
```

Also visible on a small tree with a hand-set early break, where it silently replaced the first
sector of the first file with a descriptor: eight break positions from 700 to 2000 sectors are now
byte-exact where four of them were not.

### Fixed: a layer-break guard could land inside a 3D chunk

3D only, and only with a guard in use. The guard pad is laid down wherever the copy write that reaches the zone happens to begin, and a
chunk larger than the 16 MB copy buffer takes several writes, so the pad could fall inside a chunk.
The interrupted view then gained a piece its partner did not have and the group could not be
rebuilt at all. Reproduced on real chunk tables, which have 125 chunks over 16 MB in the main
title alone:

```
Can't interleave BDMV/STREAM/SSIF/00019.ssif: 5 pieces against 4
```

The pad is now placed at the boundary between one pair of chunks and the next, where both views are
between pieces and stay in step. Verified with the pad falling both before a group and inside one,
byte exact in each case.

## tsMuxeR 2.18.3

One fault in the interface that made four options do nothing, and a new message when a 3D disc's
video is being written into the image twice.

### Fixed: the dual-layer options never reached the muxer

The meta text is what the muxer is actually handed. Not one control in the Dual-Layer group was
connected to what rebuilds it, so ticking the layer-break guard, choosing a disc in Fit to disc, or
allowing oversize changed nothing at all: the mux ran with the text as it stood. The option only
appeared once something else rebuilt the meta, such as switching the output between ISO and folder,
which is why toggling that looked like a cure.

Four options were dead this way:

```
--allow-oversize      ticked, absent, and the mux refused anyway
--disc-size           chosen, absent, so no capacity check at all
--layer-break-guard   ticked, absent, so no guard was written
--layer-break-lbn     worked out from the chosen disc, absent with it
```

Eight controls are connected now, the pattern every other control in that window already uses.

Checked by driving the window itself, selecting Blu-ray ISO and never touching the output type
afterwards: on 2.18.2 ticking the guard leaves the meta line unchanged, and with this it gains
`--layer-break-guard=288`, and `--allow-oversize` with it.

Then every one of the 46 toggles in the window was tried, on all eight tabs and on two different
source files, to find out whether anything else was in the same state. Nothing was.

The BDMV to ISO tab was never affected, because it builds its command line directly and does not
use the meta. That is why that route kept working.

Reported by @DreckSoft, who also found that it explained a layer-break guard that appeared to do
nothing.

### New: it says when a 3D disc's video is being written twice

A 3D Blu-ray stores its video once and gives it three names. The base view and the dependent view
are cut into chunks and written alternately, and that run of sectors is what the `.ssif` names; the
same sectors are addressed again as two `.m2ts`, so a 2D player opens the base one and never sees
the rest.

BDMV to ISO keeps everything under BDMV, so all three are copied as ordinary files and the video
goes into the image twice. On a pressed 3D disc measured here, 32,586 MB of a 46 GB title, which
fits no disc, least of all the one it came from.

**This release only reports it**, naming the file and how much of the image is duplicated. Nothing
is written differently yet, so an image built with this version is exactly what 2.18.2 built.

It is here because it is the first step of a feature being built: writing the video once, so a 3D
disc taken to an image comes out the size it should be. That work is underway and the message is
the part that was ready. If you have been wondering why a 3D image came out roughly double, this
is the answer, and the fix is coming.

---

## tsMuxeR 2.18.2

Two fixes, and both are about the program saying what it did rather than doing something different.
Every image and every disc this version writes is byte for byte what 2.18.1 wrote.

### Fixed: a layer break guard that was asked for and never written

The guard fills the sectors around a layer break with zeros, so that no part of the film sits where a
burn is most likely to fail. It acts only when a write actually reaches the zone around the break,
and if the break falls past the end of the image, nothing is written. Nothing was said either.

That is the ordinary outcome of leaving **Fit to disc** at **Off**: with no disc chosen the break
falls back to half a BD50, sector 12,219,392 or about 25 GB, so an image smaller than that never
reaches it. The image then comes out the same size it would have been without the option, which from
outside looks exactly like the guard not working. Measured on one source, one option apart:

```
no guard                                        2,004,418,560 bytes
--layer-break-guard=288                         2,004,418,560 bytes, and silent
the same, with the break inside the content     2,327,052,288 bytes, 307.7 MiB of guard
```

The first two being identical is the whole report. It now says which sector the image ends at, which
sector the break is at, and the two ways to fix it: set Fit to disc to the disc you are burning, or
give the break directly with `--layer-break-lbn`. A mux where the guard does land is unaffected; the
warning is raised only when no zone was written at all.

Reported by @DreckSoft.

### Fixed: a layer break guard asked for with a folder destination

The guard reserves sector positions, and only the disc image writer places sectors, so a folder
destination has nothing for it to act on. The interface greys those controls out for folder output,
but a meta file or a command line could still pass the option, and it was then ignored without a
word. It now says so, and names the folder to ISO route, where the guard does apply.

---

## tsMuxeR 2.18.1

Everything here is long standing. Two faults are about a disc layout that no version has ever
handled correctly, one is a feature that has not worked in this fork at all, and the rest are in
what the interface shows and what the program reports about a file.

### Fixed: a disc that delivers its AC-3 core in groups

A TrueHD track carries an AC-3 core braided into the same PID. Every disc tested here delivers one
core frame at a time, between 38 and 39 TrueHD units apart. A disc reported in issue 10 delivers
them in groups of three, and on that layout two separate things went wrong.

**Most of the core was stamped from the wrong clock.**

A core frame was recognised by the state the decoder was left in after sizing it, and that state
only changes when the NEXT frame is not another core frame. So in a group of three, only the last
one was recognised. The other two were stamped from the TrueHD clock and advanced it by an access
unit they do not represent, while the core's own clock advanced once per group instead of once per
frame.

Measured on the reported title, a whole feature of 94 minutes:

- of 14,207 core frames in a 7.6 minute cut, 8,476 were stamped from the wrong clock
- the core clock ran at a third speed: 182 seconds of clock for 454 seconds of audio
- 70,803 timestamps went **backwards**, the worst by 3,486 seconds, which is 58 minutes
- the error grew with the length of the film, so a longer film was worse

Afterwards the worst backward step is 0.094 seconds and it no longer grows. What remains is the
disc's own grouping: three core frames written together span three frame times, so the TrueHD unit
after them is behind. The audio itself was never affected and is byte for byte identical to the
disc either way, all 2,408,473,724 bytes of it.

**And the last frame of the track kept a timestamp from an earlier one.**

The final frame arrives through the flush path, and on such a disc it can be a core frame. It got
there still carrying an old timestamp, and it also carries a flag that suppressed the replacement,
so the stale value was written out. It was the only backward step in that output the interleave does
not account for: two frames behind on a short cut, twelve over a whole feature.

Steps between core frames are now 2,880 throughout, with no repeated timestamp anywhere.

Verified unchanged where the core arrives one frame at a time, against the released 2.18.0: a
640 kbps core with AVC video, 214,745,088 bytes, and a 448 kbps core with HEVC video, 375,742,464
bytes, both byte for byte identical. So the core bitrate is not what matters, the grouping is.

Reported and confirmed on hardware by @TexasChainsaw83.

### Fixed: play sound at end

The option has never worked in this fork, on any platform, and does work in the last release built
with the old build system. Two separate reasons.

The code is guarded by a macro that the old build system defined by itself and the current one does
not, so on every platform the sound was compiled out and a system beep was used instead.

On Windows there was also nothing to compile against. That build compiles Qt from source from its
base package alone so that the result runs on Windows 7, and sound is a separate package that is not
built. The 2.18.0 package carries four Qt libraries and no sound library at all, so it could not have
played there whatever the macro said. Windows now uses the system's own call, which takes the sound
from memory, needs nothing that is not already on the machine, and works on Windows 7 as well.

Reported by @Nemesh64, and confirmed by @oniiz86 with the detail that made it findable: that it
worked in 2.7.0 and in nothing since.

### Fixed: the interface and the progress figure

**The Merge AC-3 track box asked for a number the track list never shows.** It was written into
the meta as it stood, and the meta wants the track number in the source file, while the only
numbers on screen are the row numbers of the list. Those are not the same once a Dolby Vision file
is loaded: its two layers take two rows and share one track, so every row below them is one ahead
of its track. Reading 4 off the AC-3 row named the track after it. The box is read as a row number
now, and it explains itself when hovered. Where there is no second video row the two are the same
number, so nothing changes for a file that has none.

**And the merged track was still written on a line of its own**, which the muxer refuses, so the
interface was producing a combination it had been told to reject. That line is left out now. The
row keeps its tick: it is still in the output, just inside the TrueHD track rather than beside it.

**The progress figure could never reach the end when a track was merged in.** Those bytes reach the
reader by a different path and were counted nowhere, so the bar stopped short by exactly the share
of the file that stream takes up. On a 60 GB file with a 339 MiB AC-3 track, 0.589 percent of it,
the bar stopped at 99.4 and stayed there while the mux finished normally. Nothing written to disk
changes: the same job through 2.18.0 and through this gives byte identical output.

**The enhancement layer of a Dolby Vision file was listed at the base layer's resolution.** A
profile 7 disc taken into a Matroska file keeps both layers in one track, which is shown as two
rows, and both rows were described from the base layer. So the enhancement layer read 3840x2160
where the disc it came from says 1920x1080, and a mismatched pair looked like a matched one. Only
the reported value was ever wrong, and that was checked before anything was changed: the layer
separates out at 1920x1080 and muxes back to a disc at 1920x1080, both matching the source. The
second row is described from the enhancement layer's own parameter sets now. Reported by a tester.

### Fixed: a mux that stopped for five minutes at 75 percent

Writing a Matroska file, the muxer held every packet in memory until every track had delivered one,
so that the codec readers were initialised before the track headers were written. Nothing bounded
that wait.

On a pressed disc whose third subtitle track has its first packet 74.99 percent of the way into a
72 GB title, that meant holding three quarters of the film in memory: 42 GB on a machine with 64 GB,
with the output file not created at all and not one byte written. It then spent 178 seconds writing
those packets out and 127 more freeing them. Nothing was read during either, so the progress figure
sat at 75.0 percent for five minutes and the program looked hung.

The percentage where it stops is not a threshold in the program. It is where that subtitle track
begins, so a different disc stops at a different number, and a machine with less memory stops
earlier still.

Subtitle tracks are no longer waited for. A subtitle track entry is complete before any packet
arrives, nothing in it is discovered by parsing, and it is the one kind of track that legitimately
carries nothing for a long stretch, because a forced subtitle track only has content in the scenes
that need it. Video and audio are still waited for, because their sample rate, channel count and
even their codec id are not known until the first frame.

The same title, the same meta file, before and after:

```
                    before          after
wall time           9 min 11 s      3 min 49 s
longest pause       305.4 s         1.0 s
peak memory         42.95 GB        0.054 GB
output              60,290,004,931 bytes, both
```

The output is unchanged. Five track combinations were muxed from the same source before and after:
identical file sizes, and identical block digests, every block of every track. A Matroska file
carries randomly generated identifiers, so two runs of any one build differ in a few bytes of the
header; the two builds differ in exactly those bytes and no others.

A track of another kind that started as late would still do this. That case needs a different
answer and has not been seen.

### Also

- **Where to get the library profile 8.1 needs is now written down.** It said only that a prebuilt
  one is published. The releases page it comes from carries the command line tool beside the
  library, under a more obvious name, and the release is numbered after the tool so the library
  inside has a different version on it. Somebody copied the tool in and it did not work, which is
  the mistake the page invites. The archive to take, the one file inside it and the two traps are
  named now, in the documentation in all three languages and in the refusal message itself.

### For anyone building from source

- A build can now be started by hand from the Actions tab against any branch, so someone testing a
  fix can be given a build without a release being published. The zips are the same, and no release
  is created.

---

## tsMuxeR 2.18.0

Most of this release is about output that was wrong, or data that was missing, **and the program
did not say so**. It finished, it said "Mux successful complete", and it returned success. Ten
separate faults did that.

If you only read one part of these notes, read the next one.

### A correction to 2.17.0, first

That release said that a dual layer Dolby Vision disc taken into a Matroska file and split back out gives a Blu-ray identical to the original. It was measured on one disc and then stated for all of them. On a disc that writes its start codes in the shorter form it was not true. The rebuilt video first differs 736 bytes in.

Only the framing differed. Every picture and every piece of Dolby Vision data came back exactly, even on that disc. Both forms are correct and play the same. So a disc made with 2.17.0 works, and nobody needs to rebuild anything.

The 2.17.0 entry is corrected in place, and so is the line in the README. It is repeated here because release notes get copied to places where they cannot be edited later. The claim now holds as it was written, and the entry below has the figures, measured on a whole film.

---

### If you used an earlier version, please check these files

These faults were silent. Your files may be affected and look fine.

**1. An ISO you built with `--keep-extra-files`.**
The last part of a file could be missing. The image opens, the file is listed at the right size,
and the end of it is gone. Only extra files you added yourself are affected. The disc's own video
files are never affected.

**2. A job where you asked for `--split-size=4GB` or larger.**
You got one big file instead of several parts. Splitting was turned off, and nothing said so.

**3. An AAC audio track you demuxed from a file with cover art.**
You may have got the picture instead of the sound. One test file gave 455,104 bytes where the
audio is only 194,146 bytes.

**4. A film you joined from three or more parts.**
If the first part ended exactly on a 2 MB boundary, everything after the second part was dropped.
In one test, 20,035,292 bytes went in and 2,097,152 bytes came out.

**5. A TrueHD track you merged with an AC-3 file, written to Matroska.**
The AC-3 sound was thrown away. The file looks normal and the AC-3 is simply not there.

---

### Fixed: your data was being lost, and nothing said so

**A joined film lost everything after the second part.**

When you join files, the reader moves to the next part when a read returns less data than asked
for. A part that ends exactly on a 2 MB boundary does not do that. It returns a full block, and
then it returns nothing. That took a different path, which opened the next part and then decided
the whole list was finished.

So any part shorter than 2 MB, or an empty part, ended the join. Every part after it was never
opened. Two parts always worked, which is why nobody noticed: you need a first part that ends on
the boundary and a third part before anything is lost.

Measured on three parts of one sound track: 20,035,292 bytes in, 2,097,152 bytes out, success
reported, no warning.

**A tag in the middle of an audio file destroyed a piece of the sound.**

A file made by sticking two audio files together has a tag where they join. The part of the program
that reads a frame of sound never checked that it was looking at one. Handed the first four bytes of
a tag, it read them as a frame 314 bytes long, wrote those 314 bytes into the output as if they were
sound, and landed inside the first real frame after the tag, which was then thrown away.

That is why a small tag did damage and a large one did not: with a large tag the imaginary frame
stayed inside it.

Measured on a clean recording with a tag pushed into the middle of it, twelve tag sizes from 10
bytes to 1024. Before, anything under 314 bytes cost a whole frame of sound, about a fortieth of a
second, and part of the tag came out in the audio. Now all twelve produce exactly what the same
recording produces with no tag at all, and none of them complains.

**And a tag that begins right at the edge of a read block was reported as lost.**

The program reads a file in 2 MB pieces. A tag whose first bytes fall at the end of one piece cannot
be recognised there, and by the time the next piece arrives the program has already read past the
start of it, so the whole tag was counted as lost data on a file whose sound is complete. Measured
on a 360,989 byte tag beginning five bytes before the boundary. It now carries those few bytes over
to the next piece.

**A picture in front of an AAC track was read as sound.**

Many audio files start with a tag that holds the cover art. The marker that starts an AAC frame is
only fourteen bits long and has no checksum, so random data matches it about once every sixteen
thousand bytes. Inside a picture, that random data is very common.

A file with 1.2 MB of cover art produced 455,104 bytes of "audio" where the real audio is 194,146
bytes. It started inside the picture. The same file could not be opened at all, and was refused
with "Can't detect stream type".

The reader may now distrust a marker **inside a tag it has read and checked**, and nowhere else.
That matters: refusing markers everywhere destroys real sound. One damaged byte cost 881 bytes of
good audio in a test, and a file with junk between its frames lost 193,449 of its 194,146 bytes.
Inside a tag there is no audio to lose, so it is safe there and only there.

**An AAC track lost one frame near a block boundary.**

The reader reads in 2 MB blocks. It used the same answer for "this is not a frame" and "I do not
have enough data yet to tell". So a frame header cut in half by the end of a block was treated as
rubbish and skipped.

Measured on a file whose audio starts six bytes before the end of the first block: 193,637 bytes
instead of 194,146. Exactly 509 bytes short, which is the length of the first frame.

The same fault also made these files very slow. Four test files took over two minutes. They now
take seventeen seconds.

**A metadata tag was written into demuxed MPEG audio.**

The tag at the start or end of an MP3 was copied into the audio file as if it were sound.

**The last part of a large file was written past the end of an ISO.**

A file that does not end exactly on a sector boundary keeps its last partial sector in a buffer and
writes it separately. The position for that write was measured from the start of the file, but
added to the start of the last piece of it. Those are the same place only while the file is in one
piece.

A file is only split into pieces past one gigabyte, or where a layer break guard splits it, which
is why this survived so long. The loud version of the fault was an image about twice its correct
size. **The quiet version is the one that matters**: those last bytes went into the padding at the
end of the image, and the sector they belonged in stayed empty. The image opens. Every file inside
reads at its correct size. The end of the file is gone.

Tested with a file whose every byte can be checked, because a file full of zeros cannot tell a hole
from correct data. Before: a 2.3 GB file read back off the image differs from the original, and its
last 4096 bytes differ. After: identical.

**Asking for a 4 GB split turned splitting off.**

The split size was held in a 32 bit number. 4 GB does not fit, so it became zero, and zero meant
"do not split". 4.5 GB became 205 MB parts.

There were two faults, not one: the running total overflowed as well. Proven by building a single
part of 4,405,683,744 bytes, which the old arithmetic could not even express.

A size of zero or less is now refused, instead of quietly switching the feature off.

**A merged AC-3 sound track was thrown away in Matroska.**

A Blu-ray carries TrueHD sound as the lossless stream **plus** a normal AC-3 version, so that a
player which cannot decode TrueHD still has sound. Matroska cannot hold both in one track, so this
program writes the AC-3 as its own track.

`merge-ac3-track` and `merge-ac3-file` build the same pair from two separate sources. They use a
different reader, so the test that asks "is there an AC-3 version here" said no, and every AC-3
frame was dropped.

Measured with a 4,261,662 byte TrueHD stream and a 1,120,000 byte AC-3 file:

- before: 4,397,657 bytes out, one track. That is **the same size, to the byte**, as the TrueHD
  muxed with no AC-3 file at all
- after: 5,514,890 bytes out, two tracks

The loss happened twice over. Turning that file back into a disc gave 4,323,248 bytes instead of
7,212,432, and said nothing then either.

Both reference tools write the same shape, and the AC-3 track now decodes identically to the one
another tool produces, over all 953,856 samples it holds.

**File names on an ISO were measured in the wrong units, and names were cut short.**

An ISO stores a file name either as one byte per letter, or as two bytes per letter when the name
needs letters that do not fit in one. The writer worked out how much room to leave from the name's
size in UTF-8, which is a different number, and four things went wrong because of it.

A Cyrillic or Greek name was cut short, because each plain letter in it takes one byte in UTF-8 and
two on the image. Three Cyrillic letters in front of `_document_v1.txt` left eleven letters of
nineteen. A `v2` file beside it was cut at the same place, so both files ended up with the same
name and one of the two was lost. The mux said it was complete.

A Japanese or Chinese name was cut in the middle of a letter, leaving half a character at the end.

A name like `cafe.txt` written with an accent got an invisible zero glued to the end of it.

And a name too long for the field wrapped round instead of being refused. A 150 letter Greek name
made an image that no program will open at all. 7-Zip answers "Cannot open the file as archive",
and the mux still said it was complete.

The name is now written first and measured afterwards, so the two always agree. A name that is
still too long is cut at a whole letter and the log says which name it was and how much of it went
on the image.

An image whose names are all plain English letters is exactly the same as before.

**An ISO with more than 4093 files and folders wrecked itself, and said nothing at all.**

An ISO keeps a small table with one line per file and per folder. The room for that table was a
fixed 64 KB, which holds 4093 lines and not one more, and nothing checked. So line 4094 was written
over memory that belonged to something else.

The program then died with no message on screen at all, and left an image behind that no program
will open. Counted exactly: 4068 extra files beside a BDMV folder is 4093 lines and works, 4069 is
4094 and dies.

The table is now made as big as it needs to be, and every part of the ISO writer refuses to write
past the end of its own space instead of writing over something else.

A folder of 6000 files now builds, and all 6000 come back out with the right names and the right
contents. 20,000 works too.

**And an ISO that was too big for its own file table said "complete" instead of saying no.**

Above about 23,000 files the ISO writer runs out of a different kind of room, and it has always had
a message for that. The message never appeared, because the writer only finished its work when it
was being thrown away, and a program cannot report a problem at that moment. It either stopped dead
with nothing said, or claimed success over an unusable image.

The image is now finished before the program reports anything, so the message arrives, and it names
the numbers.

**A Dolby Vision disc listed the wrong way round lost its Dolby Vision without a word.**

A Dolby Vision disc carries the picture on two video streams: a large base layer, and a smaller
enhancement layer that holds the Dolby Vision data. They have to be listed in that order. The
program worked out which was which from the order alone, so listing them the other way round meant
the two were never joined.

Without `--dv-profile` the mux said it worked. What came out was two separate tracks with no Dolby
Vision at all, and nothing in the log said so. With `--dv-profile=8.1` it was refused, and the
refusal said that both video streams must be listed, when both were listed.

The program now recognises the wrong order and says so plainly, and the message tells you to swap
the two video lines. Files that were already right are exactly the same as before, including a disc
with a picture in picture stream beside the main video.

**A file at the root of an ISO was marked as system data.**

A file placed at the top level of an image built with `--keep-extra-files` was marked as UDF
metadata and would not open. Files inside a folder were fine, which is why this was not noticed.
Reported by @DreckSoft.

**The last picture of a stream with no end marker was lost.**

On HEVC the last picture went missing. On H.264 it was written twice. One cause, in code that six readers share. Counted from the video itself rather than from the muxer's own report, and checked on a real 3D file as well as on plain streams.

**The last frame of the AC-3 core was dropped by every merge since the feature existed.**

The tolerance in the coverage warning was worked out again with it, so a file that is now complete is not reported as short.

**An AV1 file on its own lost the start of the file and counted no frames at all.**

An AV1 file on disk is a plain chain of blocks, each one carrying its own length. It has no start
markers. tsMuxeR handles AV1 inside itself the way it handles H.264 and HEVC, which do have start
markers, and the reader looked for them in the file.

There are none. So it threw away everything up to the first three bytes that looked like a start
marker by accident, found no block boundary after that, and wrote the whole rest of the file as one
packet. Then it said "Processed 0 video frames" next to "Mux successful complete".

Measured on a file of 51,960 bytes holding 50 pictures: the first accidental marker sits 10,992
bytes in, so 10,992 bytes were dropped, one packet came out instead of fifty, and the count was
zero. AV1 inside a container was never affected.

The reader now works out how the file is laid out and reads it either way. The same file gives 50
pictures and 50 packets, a 66.8 MB file gives its 3,000, and the pictures that come back out are
the same ones that went in, to the last pixel. A file that stops in the middle of a block, or whose
blocks do not follow on from each other, now says so and gives the number of bytes it skipped.
tsMuxeR can also recognise these files now, where it used to answer "Can't detect stream type".

**And the other way round: `--demux` wrote an AV1 file that no other program will open.**

The same difference, from the other side. When you pull an AV1 track out of a disc or a transport
stream, tsMuxeR wrote the file in its own internal arrangement with the start markers still in it.
ffmpeg refuses such a file outright, and refuses it even when told to expect AV1, while it reads a
file from any encoder without complaint. The mux said it was complete.

It writes the real thing now. There was a second half to it: an AV1 file is a chain of picture
groups and each one has to begin with a small marker of its own, with nothing in front of the very
first. A transport stream carries a header ahead of that marker, so writing the pieces out in the
order they arrive still produced a file ffmpeg would not open. That header is now written just
after the marker instead, so nothing is dropped.

Measured: ffmpeg reads the file now and gets all 50 pictures, and the pictures that come out are
the same ones that went in. Take away the one header the transport stream adds and the file is
byte for byte the encoder's original. Every other kind of track comes out of a demux exactly as
before.

---

### Fixed: a refusal that destroyed the file it refused to write

Opening a file for writing empties it at once. The program created the output file **before** it
knew the job could run. So when it then refused, your existing file at that path was already gone.

**A refused image.**

Measured: 2,490,368 bytes of a good disc image replaced by a 1,769,472 byte stub.

**A rate the program cannot use.**

A muxing rate of 90 kbps or below made the program **write until the disk was full**. 89 kbps
turned a two second, 258,500 byte job into 23.9 GB. Each one reported success. The range is refused
now, and nothing is written before the refusal.

**A refused Matroska file.**

The Dolby Vision check that decides this needs the video to be read first, so it cannot run before
the file is opened. An earlier fix moved a simpler check earlier, but that check only counts video
tracks, so two video tracks passed it and the file was emptied anyway.

The output file is now created only after the real check has passed. Tested with a 96,002 byte file
at the output path, across ten different refusals: **all ten now leave it untouched**. Two of them
used to leave a 308 byte stub.

A bad output path is still reported at once, before any reading starts.

---

**Setting the frame rate on an HEVC track read one part of the stream as if it were another.**

Two kinds of header carry the frame rate, and the code that writes a new one was handed either of
them but always treated it as the first kind. The two are unrelated, so on the second kind it read
a number out of the wrong place in memory. Running the same file through the same program eight
times gave three different answers.

Most of the time that number came out as zero, and the program said "cannot override FPS in stream"
and changed nothing. Occasionally it came out as something else, said nothing, and wrote four bytes
at whatever position that number pointed at.

That is fixed: only the header that can carry a new frame rate is written to, and the other is left
alone as it always should have been. The same file now gives the same answer every time, and the
warnings that were appearing for no reason are gone.

**And the gap it exposed is closed too: `fps=` now reaches a stream that keeps its timing in the
second header.** Nothing had ever written a frame rate there, so a file that declares its rate only
in that header kept the old one no matter what you asked for. Eight bytes change and the rest of the
header does not; the decoded picture is identical, because all that changes is what the file says
about its own rate.

### Fixed: crashes

**An option written with a space.**

Writing an option with a space instead of an equals sign, for example `--split-size 4GB`, read past
the end of a list and could crash.

**A joined file on a merge-ac3 line.**

A line that joins two files and also merges an AC-3 file walked past its safety check and crashed,
after printing "Mux successful complete". The check compared the name that was typed, not the file
the line really opens, so a joined line went straight past it.

**A crash and a half core, both around the AC-3 core merge.**

A merged track that was read back could produce only part of its AC-3 core, with no warning. That is
worse than the crash beside it, because the file looks finished.

---

### Fixed: the program stayed quiet when it should have spoken

**A track that lost data now says so.**

If a track loses data on the way through, you now get a warning with the number of bytes, instead
of success and silence.

Most of the work here is in what it does **not** report, because a warning on a perfectly good file
is worse than no warning. A metadata tag is not lost sound. Counting one as lost put a warning on
ordinary tagged music: two joined MP3s reported 4,239 bytes lost on a file that is identical to the
same join without tags.

So each kind of tag is now read properly and checked, not simply believed.

**A split that leaves parts you cannot play.**

H.264 video carries its setup data once, at the start. When you split the output, every part after
the first refers to setup data that is not in it.

Measured on an 11 MB file split at 3 MB, and decoded part by part: part 1 gave 203 frames, and
parts 2, 3 and 4 all failed. **On a Blu-ray that is three unplayable clips out of four**, written
while the program reported success.

The option that fixes this already existed. What was missing was anything telling you that this job
needed it. It now warns and names the option. No output byte changes.

**Splitting was ignored for Matroska.**

The same job produced nine parts as a transport stream and one 421 MB file as Matroska, both
reporting success. It now says so, and the interface greys the split controls for Matroska.

**A merge that produced no AC-3 core at all said nothing about it.**

The warning that reports a short core opened by excluding zero, so that an average below it would not divide by zero. That left the worst outcome as the only silent one. Measured on a Matroska source, the same line differing only in the track number: a track the file holds gives 7,212,432 bytes and names the track the core came from, and a track it does not hold gives 4,323,248 bytes, no core, no warning and exit 0. That is 2,889,184 bytes of compatibility audio gone in silence, and the lost data report cannot catch it either, because a track that is not in the file contributes no bytes read and so has nothing to charge. It is easy to reach: a remuxed file does not keep the track numbers of the file it was made from, so a meta copied from the original can name a track that is not there. It now says so, and says what to check.

**`--dv-profile` was accepted and ignored on every output but Matroska.**

The option is read by the Matroska muxer and by nothing else. On a disc, a transport stream or a demux it was accepted, did nothing, and said nothing. Measured on the same source twice, differing only in whether the option is present: both files have md5 65c5268e3c4c034aad92b6e898998368. It warns now, and names the output it is talking about. A warning and not a refusal, because on those outputs the option is not wrong so much as meaningless. A disc and a transport stream carry the two layers as two streams, the way the source disc does, so there is nothing to fold and nothing to convert.

**Asking for profile 8.1 with nothing to convert.**

This did nothing at all and said nothing. It is now refused, with the reason.

**The bitrate options did not match their help text.**

Three separate problems. `--maxbitrate` was described as a limit and is not one: a 1 Mbps ceiling on
a 30.77 Mbps track gives an identical file. `--bitrate` was documented in Mbps but is read as kbps,
so the example in the help was wrong by a factor of a thousand. And `--cbr` and `--vbr` were read by
nothing at all.

All three are corrected, and three combinations that cannot mean what they look like now say so.
No output byte changes.

---

**The Linux download had no GUI in it, and nothing said so.**

Every Linux release contained the command line tool on its own. The step that builds the download
looked for the interface under a name with different capital letters from the one it is built with,
found nothing, and quietly packed the zip without it. Windows was never affected, because Windows
does not care about capital letters in a file name.

It is fixed, and it can no longer happen quietly: if the interface is missing the build now stops
instead of publishing half a release. Reported by @Frankaboy7.

### New

**A disc rebuilt from a Matroska file is now identical to the original.**

A start code is the marker between pieces of video. It can be three bytes or four. Both are legal
and both decode the same, but discs differ, and Matroska does not store start codes at all. So a
disc rebuilt through Matroska always came out in the four byte form.

The program now notes which form the source used, for each kind of piece, and writes it back the
same way. A whole 73 GB film now comes back **byte for byte identical** on both video layers,
through both Dolby Vision profiles.

This is not only a Dolby Vision matter. Any single layer disc taken to Matroska and back now keeps
its original form too.

**Dolby Vision profile 8.1, with the disc still rebuildable.**

A dual layer Dolby Vision disc can now be stored as single layer profile 8.1, which many more
devices accept, and still be turned back into the disc it came from.

The conversion cannot be reversed by calculation, so the disc's own data travels with the file as
an attachment and is put back when the file is split again. Everything the file promises is checked
before a single picture is written.

**Please note: this does not make the file smaller.** The second layer is kept so that the disc can
be rebuilt. That is the point of it.

**Profile 8.1 needs libdovi**, which is not part of tsMuxeR and is not in the downloads. On 64 bit
Windows put `dovi.dll` beside `tsMuxeR.exe`; on Linux and macOS build it, or install a package if
your distribution has one. Without it `--dv-profile=8.1` is refused before muxing starts rather
than failing part way through a feature, and the message names the library. The table in
`docs/DOLBY_VISION.md` has the detail, in English, German and Japanese.

**The index is now written at the front of the file as well.**

The index has always been written after the video, because it is not known until the end. Nothing
at the front pointed to it. That is survivable for an index and not survivable for an attachment:
other software did not list the attachments at all, and copying such a file produced one with the
video intact and the attachments quietly gone.

**An image is now checked against the disc you are burning to.**

Writing a folder to an ISO never checked whether the result would fit. It now refuses before
writing anything when the content already cannot fit, warns when the answer is close, and measures
the finished image before the run ends. `--allow-oversize` turns both into warnings.

Making that check also found `--inner-only` going over the disc by 1.44 MB in silence. It now lands
22.56 MB inside it.

**Smaller additions.**

- A merged Dolby Vision track is listed as its two layers, so you can separate it in the interface
  instead of writing a meta file by hand.
- An E-AC-3 track with Atmos now says which kind it is and how many objects it holds.
- The H.264 reader no longer assumes a four byte start code. It read the type from a fixed place, which on a short framed stream is one byte inside the header, so the type came out wrong and the muxer added a second delimiter. Checked on a real stream reframed to three bytes: 841 delimiters in, 841 out.
- The stream notes attachment has an honest name now. A Matroska file made from a source with short start codes carries a small text file recording how that source was framed. It used to be called dv-manifest.txt, which says Dolby Vision, and that name turned up on files with no Dolby Vision in them at all. The file's own first line already said "tsMuxeR stream notes", so it is tsmuxer-manifest.txt now.

---

### Interface

- A Dolby Vision selector beside the output format. It is hidden, not greyed, unless it applies.
- The selector used to do nothing at all. Choosing profile 8.1 moved the control and changed
  nothing else.
- The Blu-ray tab took no notice of the chosen output. 23 controls, and not one of them changed.
  Nothing was ever muxed wrongly; the tab was simply saying something untrue about itself.
- The Merge AC-3 file box threw away what you typed into it.
- The Max bitrate box started at 99.99 kbps, which is a real limit of 99,990 bits per second. It is
  48000 now, which gives a file identical to not using the box at all.
- The bitrate boxes could ask for a rate the program refuses.
- The Dolby Vision row appeared on a freshly opened window with nothing loaded.
- The add, join and remove buttons and the file list now have help text. The join button explains
  what it is for, which is a film split across two discs.

---

### Known limits

These are real and they are not fixed. They are listed so you know where you stand.

- A dual layer Dolby Vision pair listed with the **enhancement layer first** is still not joined
  automatically. It now says so and tells you to swap the two lines, instead of saying something
  untrue or nothing at all. Listing the base layer first works.
- The lost data warning does **not** report data lost at the very start or the very end of a track.
- A companion file longer than the track it is merged into is counted in the total, although the
  extra could never have been used.
- Splitting is not supported for Matroska output. The program now says so instead of ignoring it.
- The tag checks defend against damage, which is what happens in practice. They do not defend
  against a deliberately crafted tag.
- An ISO with more than about **23,000 files and folders** is refused, because the space set aside
  for the file table is worked out from the file count alone and does not allow for the folder
  listings. The refusal names the numbers and `--extra-iso-space` makes the space bigger. An
  ordinary disc holds a few dozen files.

---

### For anyone building from source

The version line named the commit the build folder was **configured** with, not the one that was
built. A local build here reported a commit 24 behind the code it contained. Releases were never
affected. It now updates when the code moves.

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
- Dolby Vision in Matroska, for sources with a SINGLE picture track only. Such a file now carries the configuration record a television reads to recognise Dolby Vision; without it the file played as HDR10 even though the data was there. That covers profiles 5 and 8, and it was confirmed on hardware. A DISC IS NOT COVERED AND NOW DELIBERATELY CLAIMS NOTHING: a Dolby Vision disc carries the picture on two tracks, a base layer plus a quarter resolution enhancement layer holding the Dolby Vision data, and Matroska needs the two merged into one track. That merge is not built yet, so the record is not written when a second picture track is present, and the mux says so in its log. Writing it there would leave a 1080p enhancement layer announcing itself as complete Dolby Vision when it is neither complete nor playable on its own. A disc source produces a plain HDR10 file.
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
- Down-converting a Dolby Digital Plus track that has no AC-3 core now fails with a clear message instead of writing an empty file and reporting success. Extracting the core works by keeping the AC-3 frames and discarding the E-AC-3 ones, so a DD+ stream with no core at all had nothing left to keep.
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
