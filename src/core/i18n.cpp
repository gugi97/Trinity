#include "i18n.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.h"

namespace trinity::i18n
{
    namespace
    {
        // --- String lifetime ---------------------------------------------------
        // Every pointer this module hands out must stay valid for as long as the
        // caller might hold it, and callers DO hold them: the language picker
        // keeps display names in a static array across frames, and the widgets
        // pass translated labels straight into ImGui.
        //
        // Returning std::string::c_str() out of a container would work only by
        // accident - the moment a switch cleared the table, or the language list
        // grew and its vector reallocated, every stored pointer would dangle. So
        // strings live in an arena that is only ever appended to. A deque never
        // moves the elements it already holds, so a pointer taken today is still
        // good after any number of later insertions or language switches.
        //
        // The cost is bounded and tiny: one copy of a table per switch, roughly
        // 6 KB for the current 146 strings, and nobody switches language often
        // enough for that to matter. Predictability is worth more here than the
        // handful of kilobytes.
        std::deque<std::string> g_arena;

        const char* Intern(const std::string& s)
        {
            g_arena.push_back(s);
            return g_arena.back().c_str();
        }

        struct Language
        {
            const char* code;   // interned
            const char* name;   // interned
            std::string path;   // empty for built-in English
        };

        std::vector<Language> g_langs;
        std::unordered_map<std::string, const char*> g_table; // value -> arena
        int  g_current = 0;
        bool g_discovered = false;

        // Folder holding Trinity.asi - language files sit in Languages\ beside it.
        std::string ModuleDir()
        {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&ModuleDir), &self);
            char path[MAX_PATH] = {};
            if (!GetModuleFileNameA(self, path, MAX_PATH)) return std::string();
            char* slash = strrchr(path, '\\');
            if (slash) *slash = 0;
            return std::string(path);
        }

        void Trim(std::string& s)
        {
            const size_t b = s.find_first_not_of(" \t\r\n");
            const size_t e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
        }

        // A UTF-8 BOM would otherwise become part of the first key (or of the
        // "[Language]" header), silently breaking exactly one line in a way that
        // is miserable to diagnose. Editors add it without asking, so drop it.
        void StripBom(std::string& s)
        {
            if (s.size() >= 3 &&
                static_cast<unsigned char>(s[0]) == 0xEF &&
                static_cast<unsigned char>(s[1]) == 0xBB &&
                static_cast<unsigned char>(s[2]) == 0xBF)
                s.erase(0, 3);
        }

        // Read only the [Language] header, so discovery does not parse every
        // translation in every file before one has even been chosen.
        bool ReadHeader(const std::string& path, std::string* code, std::string* name)
        {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) return false;
            char line[512];
            bool inSection = false, first = true;
            while (fgets(line, sizeof(line), f))
            {
                std::string s(line);
                if (first) { StripBom(s); first = false; }
                Trim(s);
                if (s.empty() || s[0] == ';' || s[0] == '#') continue;
                if (s[0] == '[')
                {
                    if (inSection) break;                      // past the header
                    inSection = (_stricmp(s.c_str(), "[Language]") == 0);
                    continue;
                }
                if (!inSection) continue;
                const size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                Trim(k); Trim(v);
                if (_stricmp(k.c_str(), "Code") == 0) *code = v;
                else if (_stricmp(k.c_str(), "Name") == 0) *name = v;
            }
            fclose(f);
            return !code->empty() && !name->empty();
        }

        void LoadTable(const std::string& path)
        {
            g_table.clear();          // keys are owned copies; values live in the arena
            FILE* f = fopen(path.c_str(), "rb");
            if (!f)
            {
                LOG_WARN("localisation: could not open %s - staying in English.", path.c_str());
                return;
            }
            char line[1024];
            bool inLanguageHeader = false, first = true;
            size_t n = 0, candidates = 0;
            while (fgets(line, sizeof(line), f))
            {
                std::string s(line);
                if (first) { StripBom(s); first = false; }
                Trim(s);
                if (s.empty() || s[0] == ';' || s[0] == '#') continue;
                if (s[0] == '[')
                {
                    inLanguageHeader = (_stricmp(s.c_str(), "[Language]") == 0);
                    continue;
                }
                // Split on the FIRST '=' so an English key containing '=' works.
                const size_t eq = s.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                Trim(k); Trim(v);

                // Only Name and Code belong to the [Language] header. Skipping
                // EVERY line under that header - which is what this did before -
                // silently discarded whole files, because the shipped ones put
                // their translations straight after [Language] with no second
                // section. Naming the two header keys instead means the format
                // works with or without a section for the text, which is also
                // what someone hand-editing one of these will expect.
                if (inLanguageHeader &&
                    (_stricmp(k.c_str(), "Name") == 0 || _stricmp(k.c_str(), "Code") == 0))
                    continue;

                ++candidates;
                if (k.empty() || v.empty()) continue;          // blank = not translated yet
                g_table[std::move(k)] = Intern(v);
                ++n;
            }
            fclose(f);

            // A file that parsed to nothing is a format problem, not a language
            // with no translations - say so instead of sitting silently in
            // English and letting it look like the switch did not work.
            if (n == 0)
                LOG_WARN("localisation: %s yielded no translations (%zu candidate line(s)) - "
                         "check it is 'English text=translated text', UTF-8, one per line.",
                         path.c_str(), candidates);
            else
                LOG("localisation: loaded %zu translation(s) from %s", n, path.c_str());
        }
    }

    void Discover()
    {
        if (g_discovered) return;
        g_discovered = true;

        g_langs.clear();
        g_langs.push_back({ Intern("en"), Intern("English"), "" }); // built in, never a file

        const std::string dir = ModuleDir();
        if (dir.empty()) return;

        const std::string pattern = dir + "\\Languages\\Trinity_*.ini";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            LOG("localisation: no Languages folder - English only.");
            return;
        }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string full = dir + "\\Languages\\" + fd.cFileName;
            std::string code, name;
            if (!ReadHeader(full, &code, &name))
            {
                LOG_WARN("localisation: %s has no [Language] Name/Code - skipped.", fd.cFileName);
                continue;
            }
            if (_stricmp(code.c_str(), "en") == 0) continue;   // English is built in
            g_langs.push_back({ Intern(code), Intern(name), full });
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        LOG("localisation: discovered %zu language(s) besides English.", g_langs.size() - 1);
    }

    int LanguageCount() { Discover(); return static_cast<int>(g_langs.size()); }

    const char* LanguageName(int i)
    {
        Discover();
        return (i >= 0 && i < static_cast<int>(g_langs.size())) ? g_langs[i].name : "";
    }

    const char* LanguageCode(int i)
    {
        Discover();
        return (i >= 0 && i < static_cast<int>(g_langs.size())) ? g_langs[i].code : "en";
    }

    int CurrentLanguage() { return g_current; }

    void SetLanguage(int index)
    {
        Discover();
        if (index < 0 || index >= static_cast<int>(g_langs.size())) return;
        g_current = index;
        if (g_langs[index].path.empty()) { g_table.clear(); return; }   // English
        LoadTable(g_langs[index].path);
    }

    void SetLanguageByCode(const char* code)
    {
        Discover();
        if (!code || !*code) return;
        for (size_t i = 0; i < g_langs.size(); ++i)
            if (_stricmp(g_langs[i].code, code) == 0)
            {
                SetLanguage(static_cast<int>(i));
                return;
            }
        // The saved language is no longer installed - English, not blank.
        SetLanguage(0);
    }

    bool NeedsCjkGlyphs()
    {
        Discover();
        if (g_current <= 0 || g_current >= static_cast<int>(g_langs.size())) return false;
        const char* c = g_langs[g_current].code;
        return _strnicmp(c, "zh", 2) == 0 ||
               _strnicmp(c, "ja", 2) == 0 ||
               _strnicmp(c, "ko", 2) == 0;
    }

    const char* T(const char* english)
    {
        if (!english || !*english || g_table.empty()) return english;
        const auto it = g_table.find(english);
        return (it == g_table.end()) ? english : it->second;
    }
}
