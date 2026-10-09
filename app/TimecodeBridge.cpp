#include "TimecodeBridge.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#ifndef _WIN32
#include "PosixProcess.h"
#endif
#ifdef __APPLE__
#include "mac/MacPlatform.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The worker script is deployed next to the .exe (see CMakeLists.txt's
// post-build copy). Resolving it there - not via the inherited working
// directory - matters because CreateProcess's default working directory is
// whatever the *caller's* CWD was, which is only ever the source tree root
// when launched through build.ps1 -Run; double-clicking scopedeck.exe in
// build/ gives a CWD of build/ itself, where the script already lives.
static std::string WorkerScriptPath()
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "timecode_poll_worker.py";

    std::string path(exePath, len);
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return "timecode_poll_worker.py";

    return path.substr(0, slash + 1) + "timecode_poll_worker.py";
}

static std::string ExecNoWindow(const char* cmd)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hStdOutRd, hStdOutWr;
    if (!CreatePipe(&hStdOutRd, &hStdOutWr, &sa, 0)) {
        return "";
    }
    SetHandleInformation(hStdOutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hStdOutWr;
    si.hStdError = hStdOutWr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    char cmdBuffer[1024];
    strncpy(cmdBuffer, cmd, sizeof(cmdBuffer) - 1);
    cmdBuffer[sizeof(cmdBuffer) - 1] = '\0';

    if (!CreateProcessA(NULL, cmdBuffer, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hStdOutWr);
        CloseHandle(hStdOutRd);
        return "";
    }

    CloseHandle(hStdOutWr);

    std::string result = "";
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hStdOutRd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead != 0) {
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    CloseHandle(hStdOutRd);
    WaitForSingleObject(pi.hProcess, 1000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

// Which command actually runs a real Python interpreter on this machine, if
// any - resolved once and cached, not assumed to be "python". A bare
// "python" is the one most likely to silently do nothing: on a stock Windows
// install with no Python of its own, it resolves to the Microsoft Store's
// app-execution-alias stub, which opens the Store (or, run head-less via
// CreateProcess the way this bridge does, just exits with no output) rather
// than erring in any way this code could detect. "py" is what the official
// python.org installer registers and is the more reliable signal an
// interpreter is actually there, so it is tried first.
//
// Every user this ships to needs DaVinci Resolve installed for the rest of
// the app to mean anything, but Resolve does not itself provide a Python
// interpreter - that is a separate, optional install, which is exactly why
// this cannot be assumed to exist and has to be probed for.
//
// The installer ships its own interpreter in python\ next to the exe (see
// release.ps1), so an installed copy never depends on the machine having
// one; that is tried first. A dev build has no python\ folder and falls
// through to the machine's own.
//
// Every command goes through cmd.exe /c wrapped in one extra pair of quotes:
// cmd strips the first and last quote of whatever follows /c, and without
// the wrapper that eats the quotes around a bundled path with spaces in it.
static std::string BundledPython()
{
    const std::string script = WorkerScriptPath();
    const size_t slash = script.find_last_of("\\/");
    if (slash == std::string::npos) return "";

    const std::string exe = script.substr(0, slash + 1) + "python\\python.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return "";
    return "\"" + exe + "\"";
}

static std::string ResolvePythonLauncher()
{
    const std::string bundled = BundledPython();
    const std::string candidates[] = { bundled, "py -3", "py", "python3", "python" };
    for (const std::string& candidate : candidates)
    {
        if (candidate.empty()) continue;
        const std::string probe = "cmd.exe /c \"" + candidate + " --version\"";
        const std::string result = ExecNoWindow(probe.c_str());
        if (result.find("Python") != std::string::npos)
            return candidate;
    }
    return "";   // no working interpreter found on PATH
}

// --- Persistent worker plumbing -------------------------------------------
//
// The poll thread used to spawn a brand-new "py timecode_poll_worker.py"
// process on every ~2s cycle (see TimecodePollThread below): process launch
// plus re-importing DaVinciResolveScript plus reconnecting to Resolve made
// each poll take long enough that the timeline could visibly move between
// when this thread sampled g_CurrentTimelineTime and when the worker's
// answer came back, baking that drift straight into frameOffset. Keeping
// one worker alive across the app's lifetime (talking to it over its own
// stdin/stdout instead of its one-shot exit code) makes each poll cheap
// enough that a before/after sample match (see TimecodePollThread) can be
// required before an offset is ever trusted.
//
// Owned entirely by TimecodePollThread - started, read from, and torn down
// only on that thread - so no locking is needed around these handles.
static HANDLE g_WorkerProcess  = NULL;
static HANDLE g_WorkerStdinWr  = NULL;   // our end we write requests to
static HANDLE g_WorkerStdoutRd = NULL;   // our end we read responses from
static std::string g_WorkerReadBuf;

static bool StartPersistentWorker(const std::string& launcher)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE stdinRd = NULL, stdinWr = NULL;
    HANDLE stdoutRd = NULL, stdoutWr = NULL;

    if (!CreatePipe(&stdinRd, &stdinWr, &sa, 0))
        return false;
    SetHandleInformation(stdinWr, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdoutRd, &stdoutWr, &sa, 0)) {
        CloseHandle(stdinRd);
        CloseHandle(stdinWr);
        return false;
    }
    SetHandleInformation(stdoutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput  = stdinRd;
    si.hStdOutput = stdoutWr;
    si.hStdError  = stdoutWr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Outer quotes: see ResolvePythonLauncher.
    std::string cmdStr = "cmd.exe /c \"" + launcher + " \"" + WorkerScriptPath() + "\"\"";
    std::vector<char> cmdBuffer(cmdStr.begin(), cmdStr.end());
    cmdBuffer.push_back('\0');

    BOOL ok = CreateProcessA(NULL, cmdBuffer.data(), NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    // These ends belong to the child now that it has inherited them; the
    // parent must close its copies or the pipe never reports EOF.
    CloseHandle(stdinRd);
    CloseHandle(stdoutWr);

    if (!ok) {
        CloseHandle(stdinWr);
        CloseHandle(stdoutRd);
        return false;
    }

    CloseHandle(pi.hThread);

    g_WorkerProcess  = pi.hProcess;
    g_WorkerStdinWr  = stdinWr;
    g_WorkerStdoutRd = stdoutRd;
    g_WorkerReadBuf.clear();
    return true;
}

static void StopPersistentWorker()
{
    if (g_WorkerStdinWr) {
        CloseHandle(g_WorkerStdinWr);   // EOF on the worker's stdin -> it exits its loop
        g_WorkerStdinWr = NULL;
    }
    if (g_WorkerProcess) {
        WaitForSingleObject(g_WorkerProcess, 500);
        TerminateProcess(g_WorkerProcess, 0);   // no-op if it already exited on its own
        CloseHandle(g_WorkerProcess);
        g_WorkerProcess = NULL;
    }
    if (g_WorkerStdoutRd) {
        CloseHandle(g_WorkerStdoutRd);
        g_WorkerStdoutRd = NULL;
    }
    g_WorkerReadBuf.clear();
}

// Polls hRead (via PeekNamedPipe) rather than blocking in ReadFile, so a
// hung or dead worker can be detected and torn down instead of wedging this
// thread forever. Anonymous pipes don't support overlapped I/O, which is
// the usual way to get a real timeout on Windows - this is the standard
// workaround.
static bool ReadLineWithTimeout(HANDLE hRead, std::string& outLine, DWORD timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        size_t nl = g_WorkerReadBuf.find('\n');
        if (nl != std::string::npos) {
            outLine = g_WorkerReadBuf.substr(0, nl);
            g_WorkerReadBuf.erase(0, nl + 1);
            return true;
        }

        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, NULL, 0, NULL, &avail, NULL))
            return false;   // pipe broken - worker died

        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        char buf[4096];
        DWORD toRead = avail < sizeof(buf) ? avail : sizeof(buf);
        DWORD bytesRead = 0;
        if (!ReadFile(hRead, buf, toRead, &bytesRead, NULL) || bytesRead == 0)
            return false;

        g_WorkerReadBuf.append(buf, bytesRead);
    }
}
#endif

namespace scopedeck
{

struct TimecodeInfo {
    int fps = 0;
    bool dropFrame = false;
    bool active = false;

    // Unknown until the first poll attempt resolves (or fails to resolve) a
    // Python interpreter - kept apart from fps<=0 so "Resolve isn't running
    // yet" and "there is no Python on this machine at all" read as two
    // different problems instead of the same blank dashes.
    bool pythonChecked = false;
    bool pythonFound   = false;

    // ScopeFrame::timelineTime is the OFX effect time, which counts frames
    // from 0 at the start of the timeline - not Resolve's own displayed
    // timecode, which starts from the project's Start Timecode setting
    // (01:00:00:00 by default, but a per-project setting, not a fixed
    // constant to add). frameOffset is worker_frame - timelineTime at the
    // moment of a successful poll, i.e. "how many frames Resolve's own
    // count leads ours by" - added back in on every GetText call so the
    // displayed timecode matches what Resolve itself shows, whatever that
    // project's actual start timecode is.
    long long frameOffset = 0;
    bool      haveOffset  = false;
};

static std::mutex g_TcMutex;
static TimecodeInfo g_TcInfo;
static std::atomic<bool> g_TcQuit{false};
static std::thread g_TcThread;

// The main loop's most recent ScopeFrame::timelineTime, sampled by the poll
// thread right before it asks Resolve for the matching frame number - see
// TimecodeInfo::frameOffset. Plain atomic<double>, not behind g_TcMutex: it
// is one value written every render frame and read once every poll cycle,
// and a torn read here is unobservable (worst case is off by however many
// frames one render frame's timelineTime delta is, i.e. one).
static std::atomic<double> g_CurrentTimelineTime{0.0};

void TimecodeBridgeNotifyFrame(double p_TimelineTime)
{
    g_CurrentTimelineTime.store(p_TimelineTime, std::memory_order_relaxed);
}

// The worker writes json.dumps' default `"key": value`, with a space after the
// colon, so a value cannot be read at a fixed offset past its key. That is how
// drop-frame was lost: substr(pos + 13, 4) read " tru" and every drop-frame
// timeline formatted as non-drop. Skips JSON whitespace; a missing key reads
// false.
static bool JsonBoolField(const std::string& p_Line, const char* p_Key)
{
    const size_t keyPos = p_Line.find(p_Key);
    if (keyPos == std::string::npos) return false;
    size_t i = keyPos + std::strlen(p_Key);
    while (i < p_Line.size() && (p_Line[i] == ' ' || p_Line[i] == '\t')) ++i;
    return p_Line.compare(i, 4, "true") == 0;
}

#ifndef _WIN32
// The worker script ships inside the app: Contents/Resources on macOS (next
// to the binary for a bare build) - the same "resolve it from the executable,
// never from the inherited working directory" rule as WorkerScriptPath above.
static std::string PosixWorkerScriptPath()
{
#ifdef __APPLE__
    return mac::ResourcePath("timecode_poll_worker.py");
#else
    return "timecode_poll_worker.py";
#endif
}
#endif

static void TimecodePollThread()
{
    // Resolved once for the life of the thread rather than re-probed every
    // poll - it is a fixed fact about this machine's install, and re-running
    // "py --version" etc. every 2 seconds forever would just be four wasted
    // process spawns a cycle once the answer is already known.
#ifdef _WIN32
    const std::string launcher = ResolvePythonLauncher();
#else
    const std::string launcher = FindPythonLauncher();
    PosixWorker worker;
#endif
    {
        std::lock_guard<std::mutex> lock(g_TcMutex);
        g_TcInfo.pythonChecked = true;
        g_TcInfo.pythonFound   = !launcher.empty();
    }

    while (!g_TcQuit)
    {
        bool needsPoll = false;
        {
            std::lock_guard<std::mutex> lock(g_TcMutex);
            needsPoll = g_TcInfo.active;
        }

        if (needsPoll && launcher.empty())
            needsPoll = false;   // nothing to run - see pythonFound above

        if (needsPoll)
        {
#ifdef _WIN32
            if (!g_WorkerProcess)
                StartPersistentWorker(launcher);

            if (g_WorkerProcess)
            {
                // Sampled immediately before the request goes out and again
                // right after the response comes back. With a persistent
                // worker the round trip is just an IPC hop (no process
                // launch, no re-import, no reconnect), so these two samples
                // matching is the common case - and when they don't (e.g.
                // playback moved mid-poll), the reading is discarded instead
                // of committing an offset tainted by that drift. The next
                // cycle, ~300ms later, simply tries again.
                const double timelineTimeBefore = g_CurrentTimelineTime.load(std::memory_order_relaxed);

                const char* req = "poll\n";
                DWORD written = 0;
                bool ok = WriteFile(g_WorkerStdinWr, req, (DWORD)strlen(req), &written, NULL) != 0;

                std::string result;
                if (ok)
                    ok = ReadLineWithTimeout(g_WorkerStdoutRd, result, 750);

                if (!ok) {
                    // Worker died, hung, or the pipe broke - tear it down;
                    // it gets relaunched on the next cycle.
                    StopPersistentWorker();
                } else {
                    const double timelineTimeAfter = g_CurrentTimelineTime.load(std::memory_order_relaxed);
                    const bool stableSample = std::llround(timelineTimeBefore) == std::llround(timelineTimeAfter);

                    if (stableSample && result.find("\"fps\":") != std::string::npos) {
                        size_t fpsPos = result.find("\"fps\":");
                        int fps = std::stoi(result.substr(fpsPos + 6));

                        const bool drop_frame = JsonBoolField(result, "\"drop_frame\":");

                        // "frame" is the worker's own timecode -> frame-number
                        // conversion (see timecode_poll_worker.py's
                        // _timecode_to_frame), i.e. Resolve's real frame count
                        // from its project's Start Timecode - not necessarily
                        // present on an old/failed poll, so this stays optional
                        // rather than failing the whole update over it.
                        size_t framePos = result.find("\"frame\":");
                        const bool haveFrame = framePos != std::string::npos;
                        const long long workerFrame = haveFrame
                            ? std::stoll(result.substr(framePos + 8)) : 0;

                        std::lock_guard<std::mutex> lock(g_TcMutex);
                        g_TcInfo.fps = fps;
                        g_TcInfo.dropFrame = drop_frame;
                        if (haveFrame) {
                            g_TcInfo.frameOffset = workerFrame - std::llround(timelineTimeAfter);
                            g_TcInfo.haveOffset  = true;
                        }
                    }
                }
            }
#else
            // The same persistent worker and before/after sample match as the
            // Windows branch above, over posix_spawn and pipes
            // (PosixProcess.h) instead of CreateProcess. Kept as a parallel
            // branch rather than merged: the Windows plumbing is the tested
            // one and this keeps its bytes untouched.
            if (!worker.IsRunning())
                worker.Start({ launcher, PosixWorkerScriptPath() }, "");

            if (worker.IsRunning())
            {
                const double timelineTimeBefore = g_CurrentTimelineTime.load(std::memory_order_relaxed);

                std::string result;
                bool ok = worker.WriteLine("poll\n");
                if (ok)
                    ok = worker.ReadLine(result, 750, &g_TcQuit);

                if (!ok) {
                    worker.Stop();   // died, hung or the pipe broke - relaunched next cycle
                } else {
                    const double timelineTimeAfter = g_CurrentTimelineTime.load(std::memory_order_relaxed);
                    const bool stableSample = std::llround(timelineTimeBefore) == std::llround(timelineTimeAfter);

                    if (stableSample && result.find("\"fps\":") != std::string::npos) {
                        size_t fpsPos = result.find("\"fps\":");
                        int fps = std::stoi(result.substr(fpsPos + 6));

                        const bool drop_frame = JsonBoolField(result, "\"drop_frame\":");

                        size_t framePos = result.find("\"frame\":");
                        const bool haveFrame = framePos != std::string::npos;
                        const long long workerFrame = haveFrame
                            ? std::stoll(result.substr(framePos + 8)) : 0;

                        std::lock_guard<std::mutex> lock(g_TcMutex);
                        g_TcInfo.fps = fps;
                        g_TcInfo.dropFrame = drop_frame;
                        if (haveFrame) {
                            g_TcInfo.frameOffset = workerFrame - std::llround(timelineTimeAfter);
                            g_TcInfo.haveOffset  = true;
                        }
                    }
                }
            }
#endif
        }

        for (int i = 0; i < 3 && !g_TcQuit; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

#ifdef _WIN32
    StopPersistentWorker();
#else
    worker.Stop();
#endif
}

void TimecodeBridgeStart()
{
    g_TcQuit = false;
    g_TcThread = std::thread(TimecodePollThread);
}

void TimecodeBridgeStop()
{
    g_TcQuit = true;
    if (g_TcThread.joinable())
        g_TcThread.join();
}

void TimecodeBridgeSetActive(bool p_Active)
{
    std::lock_guard<std::mutex> lock(g_TcMutex);
    g_TcInfo.active = p_Active;
}

std::string TimecodeBridgeGetText(double p_TimelineTime)
{
    int  fps = 0;
    bool drop_frame = false;
    bool pythonChecked = false, pythonFound = false;
    long long frameOffset = 0;
    bool      haveOffset  = false;
    {
        std::lock_guard<std::mutex> lock(g_TcMutex);
        fps = g_TcInfo.fps;
        drop_frame = g_TcInfo.dropFrame;
        pythonChecked = g_TcInfo.pythonChecked;
        pythonFound   = g_TcInfo.pythonFound;
        frameOffset   = g_TcInfo.frameOffset;
        haveOffset    = g_TcInfo.haveOffset;
    }

    // Distinguish "no Python interpreter found on this machine" from the
    // ordinary "Resolve isn't open yet" case below - both used to show the
    // same blank dashes, which reads as broken rather than as a missing,
    // separate, optional dependency the reader can actually go install.
    if (pythonChecked && !pythonFound)
        return "No Python found";

    if (fps <= 0 || !haveOffset) return "--:--:--:--";

    // timelineTime is the OFX effect time (0 at the timeline's own start);
    // frameOffset carries it to Resolve's own frame count, which reflects
    // the project's actual Start Timecode (01:00:00:00 by default, but a
    // per-project setting - see TimecodeInfo::frameOffset's own comment for
    // why this isn't just "add one hour").
    return FormatTimecode(std::llround(p_TimelineTime) + frameOffset, fps, drop_frame);
}

std::string FormatTimecode(long long p_Frame, int p_Fps, bool p_DropFrame)
{
    const int fps = p_Fps;
    const bool drop_frame = p_DropFrame;
    if (fps <= 0) return "--:--:--:--";

    long long frame_num = p_Frame;
    if (drop_frame && (fps == 30 || fps == 60)) {
        int drop_per_minute = (fps == 30) ? 2 : 4;
        int frames_per_minute = fps * 60 - drop_per_minute;
        int frames_per_10_minutes = frames_per_minute * 10 + drop_per_minute;
        
        long long d = frame_num / frames_per_10_minutes;
        long long m = frame_num % frames_per_10_minutes;
        
        if (m > drop_per_minute) {
            frame_num += drop_per_minute * 9 * d + drop_per_minute * ((m - drop_per_minute) / frames_per_minute);
        } else {
            frame_num += drop_per_minute * 9 * d;
        }
    }
    
    int f = frame_num % fps;
    int s = (frame_num / fps) % 60;
    int m = (frame_num / (fps * 60)) % 60;
    int h = (frame_num / (fps * 3600)) % 24;
    
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d", h, m, s, drop_frame ? ';' : ':', f);
    return std::string(buf);
}

} // namespace scopedeck
