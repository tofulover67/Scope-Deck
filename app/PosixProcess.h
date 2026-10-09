// Child processes on POSIX (macOS, Linux): the counterpart of the CreateProcess /
// anonymous-pipe plumbing TimecodeBridge.cpp and SubtitleBridge.cpp carry inline
// for Windows. Both bridges share this one copy rather than each writing their
// own, which is how the Windows side ended up with two near-identical sets.
//
// Nothing here goes through a shell: every launch is posix_spawn of an explicit
// argv, so a path with spaces or quotes in it can never split into extra
// arguments.

#pragma once

#ifndef _WIN32

#include <atomic>
#include <string>
#include <vector>

#include <sys/types.h>

namespace scopedeck
{

// Runs p_Argv to completion, returning everything it wrote to stdout and
// stderr. Kills it after p_TimeoutMs and returns what arrived until then.
// Empty if it could not be started at all.
std::string RunCaptureOutput(const std::vector<std::string>& p_Argv, int p_TimeoutMs);

// Which python3 actually runs here, as an absolute path, or "" if none does.
// Probed once per bridge thread and cached there, exactly as the Windows
// bridges do with ResolvePythonLauncher - see PosixProcess.cpp for the order
// the candidates are tried in and why /usr/bin/python3 comes last.
std::string FindPythonLauncher();

// One long-lived child talked to over its stdin/stdout, line by line - the
// persistent poll worker both bridges keep alive across the app's lifetime.
// Owned by one thread; no locking inside.
class PosixWorker
{
public:
    PosixWorker() = default;
    ~PosixWorker() { Stop(); }

    PosixWorker(const PosixWorker&) = delete;
    PosixWorker& operator=(const PosixWorker&) = delete;

    // p_Cwd empty keeps the parent's working directory.
    bool Start(const std::vector<std::string>& p_Argv, const std::string& p_Cwd);

    bool IsRunning() const { return m_Pid > 0; }

    // Writes p_Line as-is (include the trailing newline). False if the pipe
    // is broken - the worker died.
    bool WriteLine(const std::string& p_Line);

    // One line (without its newline) within p_TimeoutMs. False on timeout, a
    // broken pipe, or p_Abort becoming true - all of which the caller treats
    // as "tear the worker down and relaunch next cycle".
    bool ReadLine(std::string& p_Out, int p_TimeoutMs, const std::atomic<bool>* p_Abort = nullptr);

    // EOF on the worker's stdin (it exits its loop), a short wait, then
    // SIGKILL if it is still there. Safe to call when nothing is running.
    void Stop();

private:
    pid_t       m_Pid      = -1;
    int         m_StdinFd  = -1;   // our end, written to
    int         m_StdoutFd = -1;   // our end, read from
    std::string m_ReadBuf;
};

} // namespace scopedeck

#endif // !_WIN32
