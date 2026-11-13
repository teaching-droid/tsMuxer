// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "batchRunner.h"
#include "vodCoreException.h"

#include <fs/file.h>
#include <fs/systemlog.h>
#include <types/types.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "vod_common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

using namespace std;

namespace batch
{
vector<string> splitJobLine(const string& line)
{
    vector<string> args;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= n || line[i] == '#')
        return args;
    while (i < n)
    {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= n)
            break;
        string arg;
        bool quoted = false;
        // a part in quotes may stand inside an argument, as in --report="my report.txt"
        for (; i < n && (quoted || (line[i] != ' ' && line[i] != '\t')); ++i)
        {
            if (line[i] == '"')
                quoted = !quoted;
            else
                arg += line[i];
        }
        args.push_back(arg);
    }
    return args;
}

namespace
{
// The path of this program, to run a job with: argv[0] may be a bare name found through PATH
string ownProgram(const string& argv0)
{
#ifdef _WIN32
    wchar_t path[32768];
    const DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (len > 0 && len < std::size(path))
        return toUtf8(path);
#elif defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
        return path;
#else
    char path[4096];
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0)
        return string(path, static_cast<size_t>(len));
#endif
    return argv0;
}

#ifdef _WIN32
// An argument as CreateProcess takes it, by the rules CommandLineToArgvW reads it back with: in quotes where it has a
// space or is empty, a quote inside it behind a backslash, and the backslashes before a quote doubled
wstring quoteArg(const string& arg)
{
    const vector<wchar_t> wide = toWide(arg);
    const wstring w(wide.data());
    if (!w.empty() && w.find_first_of(L" \t\"") == wstring::npos)
        return w;
    wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : w)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        if (c == L'"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}
#endif

// ** A BATCH THAT IS ASKED TO STOP STOPS ITS JOB. ** A request to end (SIGTERM, the terminal or console window being
// closed, Ctrl+Break on Windows) ended the batch where it stood, and where the request went to the batch alone, as
// kill and a scheduler send it, the job it had started ran on with nobody waiting for it. The request is passed on
// to the job, which removes its unfinished output and ends with 4; no other job is started, and the batch ends with
// 4 after its summary. Ctrl+C in a terminal goes to the batch and its job alike and is left as it was.
atomic<int> stopAsked{0};
#ifdef _WIN32
// The system tells every program of the console, the job as well: nothing to pass on. After a closed window it ends
// this program when the handler returns, so it waits for the job and the summary; after Ctrl+Break the program goes
// on by itself, and the thread is free for the next key.
BOOL WINAPI onConsoleEvent(const DWORD event)
{
    if (event == CTRL_C_EVENT)
        return FALSE;
    stopAsked.store(1);
    if (event == CTRL_BREAK_EVENT)
        return TRUE;
    Sleep(INFINITE);
    return TRUE;
}
#else
atomic<pid_t> runningJob{0};
void onStopRequest(const int sig)
{
    stopAsked.store(sig);
    if (const pid_t job = runningJob.load(); job > 0)
        kill(job, SIGTERM);
}
#endif

void watchStopRequests()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
    for (const int sig : {SIGTERM, SIGHUP})  // SIGHUP stays ignored under nohup
        if (std::signal(sig, onStopRequest) == SIG_IGN)
            std::signal(sig, SIG_IGN);
#endif
}

// Runs one job and waits for its end. Returns whether it could be started; exitCode is what it ended with.
bool runJob(const string& program, const vector<string>& args, int& exitCode)
{
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    wstring commandLine = quoteArg(program);
    for (const string& arg : args) commandLine += L" " + quoteArg(arg);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const vector<wchar_t> wideProgram = toWide(program);
    if (!CreateProcessW(wideProgram.data(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                        &process))
        return false;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    exitCode = static_cast<int>(code);
    return true;
#else
    vector<char*> argv;
    string programCopy = program;
    vector<string> copies = args;
    argv.push_back(programCopy.data());
    for (string& arg : copies) argv.push_back(arg.data());
    argv.push_back(nullptr);
    // ** NO REQUEST IS LOST WHILE A JOB STARTS OR ENDS. ** The requests are held back from before the job is made
    // until its number is known here: one that came in between ran the handler in the new process, which is still
    // this program until the job's own begins, and was gone, and the job ran to its end. The new process takes the
    // system's own way with a request (it ends) until the job has set up its own. At the end the job's number is
    // given up before the system can hand it to another process.
    sigset_t requests, before;
    sigemptyset(&requests);
    sigaddset(&requests, SIGTERM);
    sigaddset(&requests, SIGHUP);
    sigprocmask(SIG_BLOCK, &requests, &before);
    if (stopAsked.load() != 0)
    {
        // asked for after the last look at it: the job is not started
        sigprocmask(SIG_SETMASK, &before, nullptr);
        exitCode = ExitCode::Stopped;
        return true;
    }
    const pid_t child = fork();
    if (child < 0)
    {
        sigprocmask(SIG_SETMASK, &before, nullptr);
        return false;
    }
    if (child == 0)
    {
        for (const int sig : {SIGTERM, SIGHUP})
        {
            struct sigaction action
            {
            };
            if (sigaction(sig, nullptr, &action) == 0 && action.sa_handler == onStopRequest)
                signal(sig, SIG_DFL);  // one that is ignored (nohup) stays ignored
        }
        sigprocmask(SIG_SETMASK, &before, nullptr);
        execv(program.c_str(), argv.data());
        _exit(127);  // the program could not be started
    }
    runningJob.store(child);
    sigprocmask(SIG_SETMASK, &before, nullptr);
    // wait for its end without taking its number back yet: a request in between still goes to this job, or to
    // nobody, and never to another process that got the number
    siginfo_t ended{};
    while (waitid(P_PID, static_cast<id_t>(child), &ended, WEXITED | WNOWAIT) < 0)
        if (errno != EINTR)
            break;
    sigprocmask(SIG_BLOCK, &requests, &before);
    runningJob.store(0);
    int status = 0;
    bool reaped = true;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
        {
            reaped = false;
            break;
        }
    sigprocmask(SIG_SETMASK, &before, nullptr);
    if (!reaped)
        return false;
    if (WIFEXITED(status))
    {
        exitCode = WEXITSTATUS(status);
        return exitCode != 127;
    }
    // ended by the request before it had set up its own way of stopping: stopped on request all the same
    if (WIFSIGNALED(status) && stopAsked.load() != 0 && (WTERMSIG(status) == SIGTERM || WTERMSIG(status) == SIGHUP))
        exitCode = ExitCode::Stopped;
    else
        exitCode = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    return true;
#endif
}

string timeText(const int64_t seconds)
{
    char text[32];
    snprintf(text, sizeof(text), "%d:%02d:%02d", static_cast<int>(seconds / 3600), static_cast<int>(seconds / 60 % 60),
             static_cast<int>(seconds % 60));
    return text;
}
}  // namespace

int run(const string& listFile, const bool stopOnError, const string& argv0)
{
    File file;
    if (!file.open(listFile.c_str(), File::ofRead))
    {
        LTRACE(LT_ERROR, 2, "Error: the job list " << listFile << " cannot be opened.");
        return ExitCode::Source;
    }
    int64_t size = 0;
    file.size(&size);
    if (size > 16 * 1024 * 1024)
    {
        LTRACE(LT_ERROR, 2, "Error: the job list " << listFile << " is larger than 16 MB. It is a text file of lines.");
        return ExitCode::Source;
    }
    string text(static_cast<size_t>(size), '\0');
    if (size > 0 && file.read(text.data(), static_cast<uint32_t>(size)) != size)
    {
        LTRACE(LT_ERROR, 2, "Error: the job list " << listFile << " cannot be read.");
        return ExitCode::Source;
    }
    file.close();
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);  // a byte order mark, as Windows editors write it

    struct Job
    {
        int line;
        vector<string> args;
    };
    vector<Job> jobs;
    int lineNumber = 0;
    for (size_t at = 0; at <= text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == string::npos)
            end = text.size();
        string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        ++lineNumber;
        vector<string> args = splitJobLine(line);
        // a batch inside a batch would start jobs of its own beside the ones here
        for (const string& arg : args)
            if (arg == "--batch" || strStartWith(arg, "--batch="))
            {
                LTRACE(LT_ERROR, 2,
                       "Error: line " << lineNumber << " of the job list " << listFile
                                      << " has --batch. A job is one run: a meta file and an output.");
                return ExitCode::Usage;
            }
        if (!args.empty())
            jobs.push_back({lineNumber, std::move(args)});
        at = end + 1;
    }
    if (jobs.empty())
    {
        LTRACE(LT_ERROR, 2,
               "Error: the job list " << listFile << " holds no job. Each line is the arguments of one run, for "
                                      << "example: \"film.meta\" \"film.m2ts\"");
        return ExitCode::Usage;
    }

    const string program = ownProgram(argv0);
    const size_t total = jobs.size();
    vector<string> results;
    size_t failed = 0;
    bool stopped = false;
    const auto batchStart = chrono::steady_clock::now();
    watchStopRequests();
    for (size_t i = 0; i < total; ++i)
    {
        string shown;
        for (const string& arg : jobs[i].args)
            shown += (shown.empty() ? "" : " ") + (arg.find(' ') == string::npos ? arg : "\"" + arg + "\"");
        if (stopped || stopAsked.load() != 0)
        {
            results.push_back("not run   " + shown);
            ++failed;
            continue;
        }
        LTRACE(LT_INFO, 2, "");
        LTRACE(LT_INFO, 2, "=== Job " << i + 1 << " of " << total << ": " << shown);
        const auto jobStart = chrono::steady_clock::now();
        int exitCode = 0;
        const bool started = runJob(program, jobs[i].args, exitCode);
        const int64_t took = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - jobStart).count();
        string result;
        if (!started)
            result = "could not be started";
        else if (exitCode == 0)
            result = "done";
        else if (exitCode == ExitCode::ScanWarnings)
            result = "done, the scan printed warnings";
        else if (exitCode == ExitCode::Stopped)
            result = "stopped on request";
        else
            result = "failed, exit code " + to_string(exitCode);
        LTRACE(LT_INFO, 2, "=== Job " << i + 1 << " of " << total << " " << result << " (" << timeText(took) << ")");
        // a scan that printed warnings (exit code 7) did what it was run for: a job done, with its note
        const bool warned = started && exitCode == ExitCode::ScanWarnings;
        const bool ok = started && (exitCode == 0 || warned);
        // a job that was stopped on request (exit code 4) did not fail of itself: it is named so
        const bool jobStopped = started && exitCode == ExitCode::Stopped;
        if (jobStopped)
            results.push_back("stopped   " + shown);
        else
            results.push_back(
                string(ok ? "done      " : "FAILED    ") + shown +
                (ok && !warned ? "" : "  (" + (warned ? string("the scan printed warnings") : result) + ")"));
        if (!ok)
        {
            ++failed;
            if (stopOnError)
                stopped = true;
        }
    }
    const int64_t all = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - batchStart).count();
    LTRACE(LT_INFO, 2, "");
    LTRACE(LT_INFO, 2,
           "=== Batch summary: " << total - failed << " of " << total << " jobs done, " << failed
                                 << (stopAsked.load() != 0 ? " stopped or not run (" : " failed or not run (")
                                 << timeText(all) << ")");
    for (size_t i = 0; i < results.size(); ++i) LTRACE(LT_INFO, 2, "  " << i + 1 << ". " << results[i]);
    if (stopAsked.load() != 0)
    {
        LTRACE(LT_INFO, 2, "The batch was stopped on request.");
        return ExitCode::Stopped;
    }
    return failed == 0 ? ExitCode::Done : ExitCode::Batch;
}
}  // namespace batch
