#include "SubtitleBridge.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The worker script is deployed next to the .exe (see CMakeLists.txt's
// post-build copy) - same reasoning as TimecodeBridge's own WorkerScriptPath.
static std::string WorkerScriptPath()
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "subtitle_poll_worker.py";

    std::string path(exePath, len);
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return "subtitle_poll_worker.py";

    return path.substr(0, slash + 1) + "subtitle_poll_worker.py";
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

// See TimecodeBridge.cpp's identical functions for why the bundled python
// folder is tried first, then "py -3" before a bare "python", and why every command
// is wrapped in an extra pair of quotes.
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
// Same shape as TimecodeBridge's own persistent worker: one long-lived
// Python process talked to over its own stdin/stdout instead of a fresh
// spawn (plus re-import of DaVinciResolveScript, plus reconnect) every poll.
// Owned entirely by SubtitlePollThread - started, read from, and torn down
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
// thread forever - see TimecodeBridge.cpp's identical function for why.
//
// p_Abort ends the wait early: the poll below waits up to a minute, and
// without this, closing the app mid-poll waited that long too.
static bool ReadLineWithTimeout(HANDLE hRead, std::string& outLine, DWORD timeoutMs,
                                const std::atomic<bool>& p_Abort)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        size_t nl = g_WorkerReadBuf.find('\n');
        if (nl != std::string::npos) {
            outLine = g_WorkerReadBuf.substr(0, nl);
            g_WorkerReadBuf.erase(0, nl + 1);
            return true;
        }

        if (std::chrono::steady_clock::now() >= deadline || p_Abort.load(std::memory_order_relaxed))
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

// --- Wire format parsing --------------------------------------------------
//
// Not a general JSON parser - the wire format is fixed (see
// subtitle_poll_worker.py's docstring) and this only ever reads its own
// worker's output. Escapes handled are the ones that worker can actually
// emit (\\, \", \n, \r, \t); it writes UTF-8 with ensure_ascii=False, so
// anything outside ASCII arrives as raw bytes rather than a \uXXXX escape
// and needs no further decoding here.

// Advances p_Pos past `"<key>":` and any whitespace after the colon.
// json.dumps's default separators put a space there ("s": 1, not "s":1), so
// assuming either form exactly would silently fail to match a well-formed
// line - which is precisely how the first cut of this parser managed to
// find nothing at all in responses that were perfectly correct.
static bool SeekJsonValue(const std::string& p_Json, const std::string& p_Key, size_t& p_Pos)
{
    const std::string needle = "\"" + p_Key + "\":";
    const size_t at = p_Json.find(needle, p_Pos);
    if (at == std::string::npos) return false;

    p_Pos = at + needle.size();
    while (p_Pos < p_Json.size() && (p_Json[p_Pos] == ' ' || p_Json[p_Pos] == '\t'))
        ++p_Pos;
    return true;
}

// Reads the quoted string starting at p_Pos, leaving p_Pos just past its
// closing quote.
static bool ParseJsonStringAt(const std::string& p_Json, size_t& p_Pos, std::string& p_Out)
{
    if (p_Pos >= p_Json.size() || p_Json[p_Pos] != '"') return false;
    ++p_Pos;

    std::string result;
    while (p_Pos < p_Json.size() && p_Json[p_Pos] != '"')
    {
        if (p_Json[p_Pos] == '\\' && p_Pos + 1 < p_Json.size())
        {
            switch (p_Json[p_Pos + 1])
            {
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                default:   result += p_Json[p_Pos + 1]; break;
            }
            p_Pos += 2;
        }
        else
        {
            result += p_Json[p_Pos];
            ++p_Pos;
        }
    }

    if (p_Pos >= p_Json.size()) return false;   // unterminated - truncated line
    ++p_Pos;                                    // step past the closing quote
    p_Out = result;
    return true;
}

namespace scopedeck
{

// One subtitle clip's span, in Resolve's own absolute frame numbering (the
// space TimecodeBridgeGetResolveFrame converts OFX timelineTime into).
struct SubtitleCue {
    long long   start = 0;
    long long   end   = 0;
    std::string text;
};

struct SubtitleInfo {
    bool active = false;

    // Unknown until the first poll attempt resolves (or fails to resolve) a
    // Python interpreter - see TimecodeBridge's identical fields for why
    // this is worth keeping apart from an ordinary empty reading.
    bool pythonChecked = false;
    bool pythonFound   = false;

    std::vector<SubtitleCue> cues;

    // Timeline.GetStartFrame() - the absolute frame OFX's timelineTime
    // counts from 0 at, so cueFrame = timelineTime + startFrame. False
    // until a poll has actually delivered it; there is no sane default
    // (0 would silently match cues an hour off) so the overlay draws
    // nothing rather than guessing.
    long long startFrame     = 0;
    bool      haveStartFrame = false;
};

static std::mutex g_SubMutex;
static SubtitleInfo g_SubInfo;
static std::atomic<bool> g_SubQuit{false};
static std::thread g_SubThread;

// Reads a `"cues": [{"s": <int>, "e": <int>, "t": "<text>"}, ...]` array.
// False when the response carries no "cues" key at all, which is the
// worker's "nothing changed since I last sent it" reply and must leave the
// caller's existing list untouched rather than blanking it. An empty array
// is a real answer (no subtitle track, or an empty one) and returns true
// with no cues.
static bool ParseCues(const std::string& p_Json, std::vector<SubtitleCue>& p_Out)
{
    size_t pos = 0;
    if (!SeekJsonValue(p_Json, "cues", pos)) return false;

    for (;;)
    {
        size_t cursor = pos;
        if (!SeekJsonValue(p_Json, "s", cursor)) break;
        const long long start = std::strtoll(p_Json.c_str() + cursor, nullptr, 10);

        if (!SeekJsonValue(p_Json, "e", cursor)) break;
        const long long end = std::strtoll(p_Json.c_str() + cursor, nullptr, 10);

        if (!SeekJsonValue(p_Json, "t", cursor)) break;
        std::string text;
        if (!ParseJsonStringAt(p_Json, cursor, text)) break;

        SubtitleCue cue;
        cue.start = start;
        cue.end   = end;
        cue.text  = std::move(text);
        p_Out.push_back(std::move(cue));

        pos = cursor;
    }

    return true;
}

static void SubtitlePollThread()
{
#ifdef _WIN32
    const std::string launcher = ResolvePythonLauncher();
    {
        std::lock_guard<std::mutex> lock(g_SubMutex);
        g_SubInfo.pythonChecked = true;
        g_SubInfo.pythonFound   = !launcher.empty();
    }
#endif

    while (!g_SubQuit)
    {
        bool needsPoll = false;
        {
            std::lock_guard<std::mutex> lock(g_SubMutex);
            needsPoll = g_SubInfo.active;
        }

#ifdef _WIN32
        if (needsPoll && launcher.empty())
            needsPoll = false;   // nothing to run - see pythonFound above
#endif

        if (needsPoll)
        {
            std::string result;
#ifdef _WIN32
            if (!g_WorkerProcess)
                StartPersistentWorker(launcher);

            if (g_WorkerProcess)
            {
                const char* req = "poll\n";
                DWORD written = 0;
                bool ok = WriteFile(g_WorkerStdinWr, req, (DWORD)strlen(req), &written, NULL) != 0;

                // Ordinary polls answer in single-digit milliseconds, but
                // the first read of any subtitle track costs ~3 scripting
                // round trips per cue (see subtitle_poll_worker.py's
                // _read_cues). Measured on a 1,100-cue track: ~0.45s with
                // Resolve idle, but ~18s during playback, when Resolve has
                // the playback engine busy and services scripting calls
                // perhaps forty times slower.
                //
                // This has to outlast that by a wide margin. At 15s it did
                // not: switching subtitle tracks mid-playback forced a full
                // re-read, the timeout fired partway through, the worker was
                // killed as unresponsive, and its replacement began the same
                // doomed read - so a track switch during playback never
                // completed at all, while the identical switch while paused
                // finished in half a second. Waiting costs nothing here but
                // slower detection of a genuinely dead worker: this is a
                // dedicated thread, the cue list stays on screen throughout,
                // and a worker that really has died still gets replaced.
                if (ok)
                    ok = ReadLineWithTimeout(g_WorkerStdoutRd, result, 60000, g_SubQuit);

                if (!ok) {
                    // Worker died, hung, or the pipe broke - tear it down;
                    // it gets relaunched on the next cycle.
                    //
                    // The cues deliberately survive that. They are static
                    // data (ranges and a start frame, matched locally per
                    // frame), so a worker being slow says nothing about
                    // whether they are still correct - and dropping them
                    // here is what turned one slow poll into a visibly
                    // blank overlay for as long as a fresh worker took to
                    // re-read the whole track. A relaunched worker always
                    // re-sends the full list anyway (see its rev
                    // handshake), so the worst case is briefly holding
                    // cues that are about to be replaced by identical ones.
                    StopPersistentWorker();
                    result.clear();
                }
            }
#else
            // Non-Windows still spawns one-shot per poll, same as
            // TimecodeBridge's own non-Windows path (see MAC_PORTING.md).
            FILE* pipe = _popen("python3 subtitle_poll_worker.py --once", "r");
            if (pipe) {
                char buffer[4096];
                while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    result += buffer;
                }
                _pclose(pipe);
            }
#endif
            // A failure line ({"error": ...}) clears the cues outright -
            // Resolve, the project, the timeline or the track that produced
            // them is gone, and leaving the last reading up would keep
            // showing captions for a timeline that is no longer open. A
            // plain {"rev": N} with no "cues" is the opposite case: nothing
            // changed since the worker last sent the list, so whatever is
            // already held stays exactly as it is.
            // An error line means Resolve, the project or the timeline
            // wasn't reachable on that poll. The cues stay put for the same
            // reason they survive a dead worker above: they are static, and
            // a momentary failure is not evidence they became wrong. If the
            // timeline really is gone there is no picture to draw over
            // either, and the worker re-sends the full list (having
            // forgotten what it last sent) as soon as it recovers.
            if (result.find("\"error\":") == std::string::npos)
            {
                size_t startPos = 0;
                long long startFrame = 0;
                const bool haveStart = SeekJsonValue(result, "start", startPos);
                if (haveStart)
                    startFrame = std::strtoll(result.c_str() + startPos, nullptr, 10);

                std::vector<SubtitleCue> cues;
                const bool haveCues = ParseCues(result, cues);

                std::lock_guard<std::mutex> lock(g_SubMutex);
                if (haveStart)
                {
                    g_SubInfo.startFrame     = startFrame;
                    g_SubInfo.haveStartFrame = true;
                }
                if (haveCues)
                    g_SubInfo.cues = std::move(cues);
            }
        }

        // Note there is deliberately no "else clear the cues when inactive"
        // branch. The worker only re-sends its cue list when that list has
        // changed (its rev handshake), so anything that drops cues on this
        // side without the worker knowing leaves the two permanently out of
        // step: the worker keeps answering "nothing changed" and the overlay
        // stays blank for good. Turning subtitles off and on again used to
        // do exactly that, as did a failed poll and a worker restart before
        // them. Cues are static data - they are only ever replaced here,
        // never discarded - so switching the overlay back on shows the last
        // known cues immediately and the next poll corrects them if the
        // timeline moved on meanwhile.
        for (int i = 0; i < 3 && !g_SubQuit; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

#ifdef _WIN32
    StopPersistentWorker();
#endif
}

void SubtitleBridgeStart()
{
    g_SubQuit = false;
    g_SubThread = std::thread(SubtitlePollThread);
}

void SubtitleBridgeStop()
{
    g_SubQuit = true;
    if (g_SubThread.joinable())
        g_SubThread.join();
}

void SubtitleBridgeSetActive(bool p_Active)
{
    std::lock_guard<std::mutex> lock(g_SubMutex);
    g_SubInfo.active = p_Active;
}

std::string SubtitleBridgeGetText(double p_TimelineTime)
{
    std::lock_guard<std::mutex> lock(g_SubMutex);

    if (!g_SubInfo.haveStartFrame)
        return "";

    // p_TimelineTime is the OFX effect time of the frame being drawn right
    // now, and startFrame is the absolute frame it counts from - so this is
    // exactly the frame on screen, expressed in the space the cues use. No
    // sampling, no offset inferred from where Resolve's playhead happens to
    // be relative to what the app has received: the caption is picked for
    // the picture it is being drawn over.
    const long long cueFrame = std::llround(p_TimelineTime) + g_SubInfo.startFrame;

    std::string text;
    for (const SubtitleCue& cue : g_SubInfo.cues)
    {
        if (cue.start <= cueFrame && cueFrame < cue.end)
        {
            if (!text.empty()) text += '\n';
            text += cue.text;
        }
    }
    return text;
}

} // namespace scopedeck
