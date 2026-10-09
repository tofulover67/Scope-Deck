#include "PosixProcess.h"

#ifndef _WIN32

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace scopedeck
{

namespace
{

struct Pipe
{
    int readEnd  = -1;
    int writeEnd = -1;

    bool Open()
    {
        int fds[2];
        if (::pipe(fds) != 0) return false;
        readEnd  = fds[0];
        writeEnd = fds[1];
        // Neither end leaks into any other child this process spawns; the
        // one child that should have an end gets it through dup2 below.
        ::fcntl(readEnd,  F_SETFD, FD_CLOEXEC);
        ::fcntl(writeEnd, F_SETFD, FD_CLOEXEC);
        return true;
    }

    void CloseRead()  { if (readEnd  >= 0) { ::close(readEnd);  readEnd  = -1; } }
    void CloseWrite() { if (writeEnd >= 0) { ::close(writeEnd); writeEnd = -1; } }
    ~Pipe() { CloseRead(); CloseWrite(); }
};

// posix_spawn of p_Argv with the given stdin/stdout, stderr joined to stdout
// (the bridges read everything the worker prints, as on Windows). p_Cwd ""
// keeps ours.
pid_t Spawn(const std::vector<std::string>& p_Argv, int p_StdinFd, int p_StdoutFd, const std::string& p_Cwd)
{
    if (p_Argv.empty()) return -1;

    std::vector<char*> argv;
    argv.reserve(p_Argv.size() + 1);
    for (const std::string& a : p_Argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (p_StdinFd >= 0)  posix_spawn_file_actions_adddup2(&actions, p_StdinFd,  STDIN_FILENO);
    else                 posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (p_StdoutFd >= 0)
    {
        posix_spawn_file_actions_adddup2(&actions, p_StdoutFd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, p_StdoutFd, STDERR_FILENO);
    }
#ifdef __APPLE__
    // Apple's extension; the portable addchdir_np landed in POSIX 2024 and
    // not every SDK has it yet.
    if (!p_Cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, p_Cwd.c_str());
#else
    (void)p_Cwd;
#endif

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0 ? pid : -1;
}

// Reaps p_Pid, SIGKILLing it first if it is still running after p_GraceMs.
void Reap(pid_t p_Pid, int p_GraceMs)
{
    if (p_Pid <= 0) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_GraceMs);
    for (;;)
    {
        int status = 0;
        const pid_t r = ::waitpid(p_Pid, &status, WNOHANG);
        if (r == p_Pid || (r < 0 && errno != EINTR)) return;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ::kill(p_Pid, SIGKILL);
            ::waitpid(p_Pid, &status, 0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool IsExecutable(const std::string& p_Path)
{
    struct stat st;
    return ::stat(p_Path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p_Path.c_str(), X_OK) == 0;
}

} // namespace

std::string RunCaptureOutput(const std::vector<std::string>& p_Argv, int p_TimeoutMs)
{
    Pipe out;
    if (!out.Open()) return "";

    const pid_t pid = Spawn(p_Argv, -1, out.writeEnd, "");
    out.CloseWrite();   // ours would otherwise keep the pipe from ever reporting EOF
    if (pid <= 0) return "";

    std::string result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_TimeoutMs);
    for (;;)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;

        pollfd pfd{ out.readEnd, POLLIN, 0 };
        const int ready = ::poll(&pfd, 1, int(std::min<long long>(left, 100)));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;

        char buf[4096];
        const ssize_t n = ::read(out.readEnd, buf, sizeof(buf));
        if (n <= 0) break;   // EOF (or error): the child closed its end
        result.append(buf, size_t(n));
    }

    Reap(pid, 200);
    return result;
}

std::string FindPythonLauncher()
{
    std::vector<std::string> candidates;

    // The installer puts python.org's Python here when the Mac has none
    // (installer/mac/postinstall), and that is also where a hand-installed
    // one lives. Newest first; "Current" is whichever the user picked.
    candidates.push_back("/Library/Frameworks/Python.framework/Versions/Current/bin/python3");
    if (DIR* d = ::opendir("/Library/Frameworks/Python.framework/Versions"))
    {
        std::vector<std::string> versions;
        while (dirent* e = ::readdir(d))
        {
            const std::string name = e->d_name;
            if (name.size() > 2 && name[0] == '3' && name[1] == '.') versions.push_back(name);
        }
        ::closedir(d);
        std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b)
        {
            return std::atoi(a.c_str() + 2) > std::atoi(b.c_str() + 2);
        });
        for (const std::string& v : versions)
            candidates.push_back("/Library/Frameworks/Python.framework/Versions/" + v + "/bin/python3");
    }

    candidates.push_back("/opt/homebrew/bin/python3");
    candidates.push_back("/usr/local/bin/python3");

    // Last, and only when a real interpreter sits behind it: on a Mac without
    // the Command Line Tools, /usr/bin/python3 is a stub that opens a dialog
    // offering to install them - from a headless poll thread, every two
    // seconds, forever. Same trap as the Microsoft Store alias on Windows.
    if (IsExecutable("/Library/Developer/CommandLineTools/usr/bin/python3") ||
        IsExecutable("/Applications/Xcode.app/Contents/Developer/usr/bin/python3"))
        candidates.push_back("/usr/bin/python3");

    for (const std::string& c : candidates)
    {
        if (!IsExecutable(c)) continue;
        const std::string out = RunCaptureOutput({ c, "--version" }, 3000);
        if (out.find("Python 3") != std::string::npos) return c;
    }
    return "";
}

bool PosixWorker::Start(const std::vector<std::string>& p_Argv, const std::string& p_Cwd)
{
    Stop();

    Pipe toChild, fromChild;
    if (!toChild.Open() || !fromChild.Open()) return false;

    const pid_t pid = Spawn(p_Argv, toChild.readEnd, fromChild.writeEnd, p_Cwd);
    // The child's ends belong to it now; closing our copies is what makes
    // EOF reach either side when the other goes away.
    toChild.CloseRead();
    fromChild.CloseWrite();
    if (pid <= 0) return false;

    m_Pid      = pid;
    m_StdinFd  = toChild.writeEnd;    toChild.writeEnd  = -1;
    m_StdoutFd = fromChild.readEnd;   fromChild.readEnd = -1;
    m_ReadBuf.clear();
    return true;
}

bool PosixWorker::WriteLine(const std::string& p_Line)
{
    if (m_StdinFd < 0) return false;
    const char* p = p_Line.data();
    size_t left = p_Line.size();
    while (left > 0)
    {
        // A dead worker means EPIPE here, not a signal: SIGPIPE is ignored
        // for the whole process in main() on this platform.
        const ssize_t n = ::write(m_StdinFd, p, left);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        left -= size_t(n);
    }
    return true;
}

bool PosixWorker::ReadLine(std::string& p_Out, int p_TimeoutMs, const std::atomic<bool>* p_Abort)
{
    if (m_StdoutFd < 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_TimeoutMs);

    for (;;)
    {
        const size_t nl = m_ReadBuf.find('\n');
        if (nl != std::string::npos)
        {
            p_Out = m_ReadBuf.substr(0, nl);
            m_ReadBuf.erase(0, nl + 1);
            return true;
        }

        if (p_Abort && p_Abort->load(std::memory_order_relaxed)) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;

        // Short poll slices so an abort (the app closing mid-poll) is seen
        // within a frame or two even against a minute-long timeout.
        pollfd pfd{ m_StdoutFd, POLLIN, 0 };
        const int ready = ::poll(&pfd, 1, int(std::min<long long>(left, 50)));
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) continue;
        if (ready < 0) return false;

        char buf[4096];
        const ssize_t n = ::read(m_StdoutFd, buf, sizeof(buf));
        if (n <= 0) return false;   // EOF or error: the worker died
        m_ReadBuf.append(buf, size_t(n));
    }
}

void PosixWorker::Stop()
{
    if (m_StdinFd >= 0)
    {
        ::close(m_StdinFd);   // EOF on the worker's stdin -> it leaves its loop
        m_StdinFd = -1;
    }
    if (m_Pid > 0)
    {
        Reap(m_Pid, 500);
        m_Pid = -1;
    }
    if (m_StdoutFd >= 0)
    {
        ::close(m_StdoutFd);
        m_StdoutFd = -1;
    }
    m_ReadBuf.clear();
}

} // namespace scopedeck

#endif // !_WIN32
