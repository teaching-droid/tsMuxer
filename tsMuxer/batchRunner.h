// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef BATCH_RUNNER_H_
#define BATCH_RUNNER_H_

#include <string>
#include <vector>

// ** A LIST OF JOBS, ONE AFTER THE OTHER (--batch). ** A series of recordings or discs had to be started one by one,
// or by a script that the user wrote. A job list is a text file in UTF-8 with one job on each line, written as the
// arguments of a tsMuxeR run of its own: a meta file and an output, and any option a run takes. Empty lines and lines
// that begin with # are passed over.
//
// Each job is run as a process of its own, by this same program, so one job cannot change another: a job that stops
// with an error, or crashes, leaves the next ones as they are. The jobs run strictly one after the other. What each
// job prints goes to the same output, between a line that names the job and a line that gives its result, and a
// summary at the end says how each job ended.
namespace batch
{
// One line of a job list as the arguments it holds: split at spaces and tabs, a part in double quotes kept whole,
// the quotes taken off. An empty line, and a line that begins with #, give none.
std::vector<std::string> splitJobLine(const std::string& line);

// Runs the jobs of the list file. stopOnError ends the batch at the first job that fails; the jobs after it are named
// as not run. Returns 0 when every job ended with exit code 0, -1 when one failed or was not run, and -2 when the
// list cannot be read or holds no job.
int run(const std::string& listFile, bool stopOnError, const std::string& argv0);

// The file of this program
std::string programFile();

// Starts a helper program with its arguments and waits for its end, for `seconds` at most. It gets no input, what
// it prints is not shown, and it opens no window. Returns 0 when it ended (exitCode is what it ended with), 1 when
// it could not be started, 2 when it was ended here because it ran longer than the limit.
int runHelper(const std::string& program, const std::vector<std::string>& args, int seconds, int& exitCode);
}  // namespace batch

#endif
