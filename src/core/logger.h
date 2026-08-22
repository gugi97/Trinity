#pragma once
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <deque>
#include <mutex>
#include <string>

namespace trinity
{
    // Console logger. The console is created lazily (only the process that
    // actually renders the game calls EnableConsole), so the launcher process
    // that also loads the ASI stays silent. Lines logged before the console
    // exists are buffered and flushed into it on creation.
    //
    // The same lines are optionally mirrored to Trinity.log next to
    // Trinity.asi (EnableFile). The console is transient - it dies with the
    // game and cannot be scrolled back far - whereas signature-resolve
    // failures, which are the single most useful thing this mod prints, all
    // happen during startup. Having them on disk is what makes diagnosing a
    // game patch possible after the fact.
    class Logger
    {
    public:
        enum Level { Info, Good, Warn, Error };

        static void EnableConsole()
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (s_console)
                return;

            AllocConsole();
            freopen_s(&s_conFp, "CONOUT$", "w", stdout);
            SetConsoleTitleA("Trinity");
            s_console = true;

            for (const auto& line : s_buffer)
                Emit(line);
            s_buffer.clear();
        }

        // Mirror the log to Trinity.log next to Trinity.asi. Call this from the
        // same place as EnableConsole (the process that presents) so the
        // launcher's copy of the ASI never truncates the file we are writing.
        // Buffered startup lines are replayed into the file WITHOUT clearing
        // the buffer, so a later EnableConsole can still replay them too.
        // Stop writing Trinity.log and release the handle. Kept separate from
        // Shutdown so the setting can be turned back on in the same session.
        static void DisableFile()
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (s_fileFp) { std::fclose(s_fileFp); s_fileFp = nullptr; }
        }

        static bool FileEnabled()
        {
            std::lock_guard<std::mutex> lock(Mutex());
            return s_fileFp != nullptr;
        }

        static void EnableFile()
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (s_fileFp)
                return;

            // Resolve our own module (Trinity.asi) from the address of this
            // function, so the logger keeps no dependency on Mod (which would
            // be an include cycle: mod.cpp -> logger.h -> mod.h).
            HMODULE self = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(&EnableFile), &self))
                return;

            char path[MAX_PATH];
            const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return;

            char* slash = std::strrchr(path, '\\');
            if (!slash)
                return;

            const size_t left = sizeof(path) - static_cast<size_t>(slash + 1 - path);
            if (snprintf(slash + 1, left, "Trinity.log") >= static_cast<int>(left))
                return;

            // _fsopen, not fopen_s: fopen_s opens EXCLUSIVE on MSVC, which
            // makes the log unreadable while the game is running - exactly
            // when you want to read it. _SH_DENYWR lets anyone tail the file
            // and still keeps a second process from writing into it.
            s_fileFp = _fsopen(path, "w", _SH_DENYWR);
            if (!s_fileFp)
                return;

            for (const auto& line : s_buffer)
                EmitFile(line);
            std::fflush(s_fileFp);
        }

        static void Shutdown()
        {
            std::lock_guard<std::mutex> lock(Mutex());
            if (s_fileFp)  { std::fclose(s_fileFp); s_fileFp = nullptr; }
            if (s_conFp)   { fclose(s_conFp); s_conFp = nullptr; }
            if (s_console) { FreeConsole(); s_console = false; }
        }

        static void Log(Level lvl, const char* fmt, ...)
        {
            char msg[1024];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(msg, sizeof(msg), fmt, ap);
            va_end(ap);

            Line line;
            line.lvl = lvl;
            SYSTEMTIME st;
            GetLocalTime(&st);
            char stamp[16];
            snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
            line.stamp = stamp;
            line.text  = msg;

            std::lock_guard<std::mutex> lock(Mutex());

            if (s_fileFp)
                EmitFile(line);

            if (s_console)
            {
                Emit(line);
            }
            else
            {
                s_buffer.emplace_back(std::move(line));
                if (s_buffer.size() > 256)
                    s_buffer.pop_front();
            }
        }

    private:
        struct Line { Level lvl = Info; std::string stamp, text; };

        static void Emit(const Line& l)
        {
            HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            WORD body = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
            switch (l.lvl)
            {
            case Good:  body = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Warn:  body = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Error: body = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
            default: break;
            }

            SetConsoleTextAttribute(h, FOREGROUND_INTENSITY);
            std::printf("%s ", l.stamp.c_str());
            SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::printf("Trinity ");
            SetConsoleTextAttribute(h, body);
            std::printf("%s\n", l.text.c_str());
            SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            std::fflush(stdout);
        }

        // Plain-text mirror of Emit. No colour codes - the level is spelled out
        // so the file stays greppable ("grep ERR Trinity.log").
        static void EmitFile(const Line& l)
        {
            const char* tag = "INFO";
            switch (l.lvl)
            {
            case Good:  tag = "OK  "; break;
            case Warn:  tag = "WARN"; break;
            case Error: tag = "ERR "; break;
            default: break;
            }
            std::fprintf(s_fileFp, "%s [%s] %s\n", l.stamp.c_str(), tag, l.text.c_str());
            std::fflush(s_fileFp); // a crash mid-hook must not eat the last line
        }

        static std::mutex& Mutex()
        {
            static std::mutex m;
            return m;
        }

        static inline FILE*            s_conFp   = nullptr;
        static inline FILE*            s_fileFp  = nullptr;
        static inline bool             s_console = false;
        static inline std::deque<Line> s_buffer;
    };
}

#define LOG(...)      ::trinity::Logger::Log(::trinity::Logger::Info,  __VA_ARGS__)
#define LOG_OK(...)   ::trinity::Logger::Log(::trinity::Logger::Good,  __VA_ARGS__)
#define LOG_WARN(...) ::trinity::Logger::Log(::trinity::Logger::Warn,  __VA_ARGS__)
#define LOG_ERR(...)  ::trinity::Logger::Log(::trinity::Logger::Error, __VA_ARGS__)
