[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [Português (Portugal)](REPORTING_A_PROBLEM_PT_PT.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Reporting a problem

When a mux goes wrong, two files from tsMuxeR help to find the fault without your whole film:

| File | What it is | Size | When to send it |
|---|---|---|---|
| **Report** | A text file of the run: what tsMuxeR was given, every message, what each track read and left out, and what was written | Small | For every problem |
| **Sample** | A .zip with a part of the source, copied byte for byte | Up to 25 MB | When the report is not enough, or when you are asked for one |

Both can be made in the window or on the command line. Attach them to a new issue with the [Bug report form](https://github.com/teaching-droid/tsMuxer/issues/new/choose). You can drag a file into the form.

**An issue is public.** A report holds no picture or sound, and it replaces your file names. A sample holds a part of the film itself, so attach one only if you may share it.

## In the window

### Make a report

1. Open the **General** tab and tick **Write a report of each mux for a problem report**.
2. Run the job again, so that the problem happens.
3. In the progress window, click **Save report...** when the job has ended.
4. Read the text that is shown, then save it.

### Make a sample

1. On the **Input** tab, choose the file in the list and click **Make a sample...**. The progress window also offers this when a job ends with an error.
2. Under **Take**, choose **the start**, **the end**, or **around this time (where the problem is)**, and give the time.
3. Under **Size**, keep the size that is given, or choose a smaller one. The .zip stays under 25 MB.
4. Tick **Add the report of the last job** if there is one. **Show report** shows it first.
5. Under **Save as**, choose where the .zip goes, and click **Make sample**.

## On the command line

Add `--report` to the job that goes wrong:

```
tsMuxeR --report=report.txt film.meta film.ts
```

Then make a sample around the time of the problem, with that report inside it:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Option | What it does |
|---|---|
| `--from-start` | Takes the start of the source. This is the default. |
| `--from-end` | Takes the end of the source. |
| `--at=<time>` | Takes the part around a time, as h:mm:ss or seconds. |
| `--size=<MB>` | Sets the size of the sample, from 1 to 24 MB. It is 20 when not given. |
| `--add-report=<file>` | Puts the report of the job that went wrong into the .zip. |

The name of the report cannot be a file of the job: a source, the meta file or the output. The sample cannot be written over its source or over the report that goes into it. tsMuxeR refuses these names and says so.

## What the report holds

* the version of tsMuxeR and the system: its version, the number of processors, the memory, and the time of the run
* the command line and the meta file
* the size and the first 32 bytes of each source, and its listing
* every message of the run, with its time in seconds, also the lines that are not shown on the screen
* what each track read, used and left out
* the files written, with their sizes

**Your names stay private.** Folders are left out of every path. The names of files, titles, tracks and chapters are replaced by placeholders, such as `<source1>.mkv`. A section called Names gives only what is needed to find a fault with a name, such as its length and whether it has spaces. The names of the files of a disc, such as `00011.mpls`, are kept.

Only if you are asked to, `--report-keep-names` keeps the names and `--report-full-paths` keeps the folders too. `--report-frames` adds a second, much larger file with a line for each part of the stream that is written. Give it a time range, such as `--report-frames=0:10:00-0:12:00`.

## What the sample holds

* **a part of the source**, copied byte for byte, so that it shows the same fault as the source
* **a report of making the sample**, and the report of the job if you added one

The file is called `source1` in the .zip, followed by its own extension. A Matroska sample keeps its tracks and chapters, but its title and the names of its tracks and chapters are blanked. Its tags, attachments and index are left out.

The sample begins where a reader can begin, so it may start a little before the time you gave. A source that fits in the sample is taken whole, and a time past the end takes the end.

Some sources have limits:

| Source | Limit |
|---|---|
| MP4, MOV | Only the start can be taken, so `--at` and `--from-end` are refused. |
| A video stream on its own | It holds no times, so take its start or its end. |
| A file inside a disc image | Name it by its path after the image, as `film.iso/BDMV/STREAM/00001.m2ts`. |

For the details of each kind of source, see "Sample for a problem report" in [USAGE](USAGE.md).

## What else helps

* what you expected, and what happened instead
* for a playback problem, the player or device you used

The report already holds the version, the meta file and the whole log, so with a report you do not need to paste them.
