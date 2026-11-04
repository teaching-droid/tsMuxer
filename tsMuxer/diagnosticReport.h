#ifndef DIAGNOSTIC_REPORT_H_
#define DIAGNOSTIC_REPORT_H_

#include <cstdint>
#include <string>
#include <vector>

// ** THE DIAGNOSTIC REPORT (--report=<file>). ** A problem with a mux could only be looked into with the source file
// itself, which is often too big to send or not the sender's to give. The report is a text file to attach instead: what
// the run was given (the command line, the meta file, the size and first bytes of each source and its listing), every
// line tsMuxeR said, the lines it keeps to itself as well, each with its time, what each track read, made and left
// out, and what was written. Folders are left out of every path unless --report-full-paths is given, and the names of
// files, titles, track names and chapter names are replaced by placeholders unless --report-keep-names is given, so
// the report says nothing of the computer it came from or of what is on it. Without --report nothing of this runs and
// the output is as before.
namespace diagReport
{
extern bool active;  // a report is being written: LTRACE hands every line on to it

// Takes --report=<file>, --report-full-paths and --report-keep-names out of the arguments, as --lang-codes is taken
// out, and opens the report. Returns false, the error said, when the file cannot be written.
bool takeOptions(int& argc, char** argv);
// The start of the report: version, build, system, the command line, the meta file and the sources
void writeStart(int argc, char** argv);
// A line said with LTRACE: printed as LTRACE prints it, and written to the report
void trace(unsigned level, int errIndex, const std::string& text);
// The start of a line said without its end (LTRACE2): printed, and kept for the line that ends it
void partial(unsigned level, const std::string& text);
// The progress of a mux, one line in the report for every tenth of the way
void progress(double percent);
// A section of its own, and its lines; paths in them are shortened as everywhere
void section(const std::string& title);
void text(const std::string& line);
// While a source is listed for the report, the lines of the listing go to it as plain text
void setListing(bool on);
// A path whose folder is left out wherever it appears
void addPath(const std::string& path);
// A title, a track name or a chapter name a source holds, as it is read: replaced wherever it appears by a placeholder
// that says what it is ("title", "track name", "chapter name", "disc title") and how long it is
void addName(const std::string& text, const std::string& kind);
// A file of the run whose name the report replaces by a placeholder: kind "sample" is "<sample>"
void addFileName(const std::string& path, const std::string& kind);
// --report-keep-names or --report-full-paths was given
bool keepingNames();
// The output the report describes at its end; empty for none (a sample, which is written after the report)
void setOutput(const std::string& output);
// The sources the meta file names, in its order, each once
std::vector<std::string> metaSources();
// The run was given a meta file: a mux or a demux
bool muxRun();
// The output and the end of the run, with its exit code; a run ended by a signal says so
void writeOutput(const std::string& output);
void close(int exitCode);
void stopped(int signal);

// ** THE FRAMES OF A MUX (--report-frames[=<from>-<to>]). ** A file beside the report, <report>.frames.tsv, with a
// line for every PES a transport stream mux writes: its track, time stamps, size, the packet it begins in and the clock
// then; and for MPEG-2 video a line for every picture as the decoder's buffer model takes it out (tstdCheck). A film
// gives millions of lines, so a range of the output's time can be asked for, as h:mm:ss or seconds.
extern bool framesActive;
// The time of a frame in the output, in seconds from the first one handed here, from its 90 kHz time stamp
double frameTime(int64_t ts90);
bool framesWanted(double seconds);
void frameRow(const std::string& row);
// The same from a signal handler, where only a plain write is safe: the program ends right after
void signalNote(int signal);
}  // namespace diagReport

#endif
