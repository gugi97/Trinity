#include "i18n.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.h"

namespace trinity::i18n
{
    namespace
    {
        struct Language
        {
            std::string code;
            std::string name;
            std::string path;   // empty for built-in English
        };

        std::vector<Language> g_langs;
        std::unordered_map<std::string, std::string> g_table;
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
            size_t b = s.find_first_not_of(" \t\r\n");
            size_t e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
        }

        // Read just the [Language] header, so discovery does not parse every
        // translation in every file before one is even selected.
        bool ReadHeader(const std::string& path, std::string* code, std::string* name)
        {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) return false;
            char line[512];
            bool inSection = false;
            while (fgets(line, sizeof(line), f))
            {
                std::string s(line);
                Trim(s);
                if (s.empty() || s[0] == ';' || s[0] == '#') continue;
                if (s[0] == '[')
                {
                    if (inSection) break;                 // past the header
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
            g_table.clear();
            FILE* f = fopen(path.c_str(), "rb");
            if (!f)
            {
                LOG_WARN("localisation: could not open %s - staying in English.", path.c_str());
                return;
            }
            char line[1024];
            bool inLanguageHeader = false;
            size_t n = 0;
            while (fgets(line, sizeof(line), f))
            {
                std::string s(line);
                Trim(s);
                if (s.empty() || s[0] == ';' || s[0] == '#') continue;
                if (s[0] == '[')
                {
                    inLanguageHeader = (_stricmp(s.c_str(), "[Language]") == 0);
                    continue;
                }
                if (inLanguageHeader) continue;           // Name= / Code=, not text
                // Split on the FIRST '=' so an English key containing '=' works.
                const size_t eq = s.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                Trim(k); Trim(v);
                if (k.empty() || v.empty()) continue;     // blank = "not translated yet"
                g_table.emplace(std::move(k), std::move(v));
                ++n;
            }
            fclose(f);
            LOG("localisation: loaded %zu translation(s) from %s", n, path.c_str());
        }
    }

    void Discover()
    {
        if (g_discovered) return;
        g_discovered = true;

        g_langs.clear();
        g_langs.push_back({ "en", "English", "" });   // built in, never a file

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
            g_langs.push_back({ code, name, full });
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        LOG("localisation: discovered %zu language(s) besides English.", g_langs.size() - 1);
    }

    int LanguageCount() { Discover(); return static_cast<int>(g_langs.size()); }

    const char* LanguageName(int i)
    {
        Discover();
        return (i >= 0 && i < static_cast<int>(g_langs.size())) ? g_langs[i].name.c_str() : "";
    }

    const char* LanguageCode(int i)
    {
        Discover();
        return (i >= 0 && i < static_cast<int>(g_langs.size())) ? g_langs[i].code.c_str() : "en";
    }

    int CurrentLanguage() { return g_current; }

    void SetLanguage(int index)
    {
        Discover();
        if (index < 0 || index >= static_cast<int>(g_langs.size())) return;
        g_current = index;
        if (g_langs[index].path.empty()) { g_table.clear(); return; }  // English
        LoadTable(g_langs[index].path);
    }

    void SetLanguageByCode(const char* code)
    {
        Discover();
        if (!code || !*code) return;
        for (size_t i = 0; i < g_langs.size(); ++i)
            if (_stricmp(g_langs[i].code.c_str(), code) == 0) { SetLanguage(static_cast<int>(i)); return; }
        // Saved language is not installed any more - English rather than blank.
        SetLanguage(0);
    }

    bool NeedsCjkGlyphs()
    {
        Discover();
        if (g_current <= 0 || g_current >= static_cast<int>(g_langs.size())) return false;
        const std::string& c = g_langs[g_current].code;
        return _strnicmp(c.c_str(), "zh", 2) == 0 ||
               _strnicmp(c.c_str(), "ja", 2) == 0 ||
               _strnicmp(c.c_str(), "ko", 2) == 0;
    }

    const char* T(const char* english)
    {
        if (!english || !*english || g_table.empty()) return english;
        auto it = g_table.find(english);
        return (it == g_table.end()) ? english : it->second.c_str();
    }
}
