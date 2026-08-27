#include "i18n.h"
#include "i18n_embedded.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sstream>
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
            std::string path;   // empty for built-in English / embedded
            const TranslationEntry* embeddedEntries = nullptr;
            size_t embeddedCount = 0;
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

        void ParseLinesIntoTable(const std::vector<std::string>& lines, const char* sourceName)
        {
            g_table.clear();
            bool inLanguageHeader = false, first = true;
            size_t n = 0;
            for (std::string s : lines)
            {
                if (first) { StripBom(s); first = false; }
                Trim(s);
                if (s.empty() || s[0] == ';' || s[0] == '#') continue;
                if (s[0] == '[')
                {
                    inLanguageHeader = (_stricmp(s.c_str(), "[Language]") == 0);
                    continue;
                }
                const size_t eq = s.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                Trim(k); Trim(v);
                if (inLanguageHeader &&
                    (_stricmp(k.c_str(), "Name") == 0 || _stricmp(k.c_str(), "Code") == 0))
                    continue;

                if (k.empty() || v.empty()) continue;
                g_table[std::move(k)] = Intern(v);
                ++n;
            }
            LOG("localisation: loaded %zu translation(s) from %s", n, sourceName);
        }

        void LoadTable(const std::string& path)
        {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f)
            {
                LOG_WARN("localisation: could not open %s - staying in English.", path.c_str());
                return;
            }
            char line[1024];
            std::vector<std::string> lines;
            while (fgets(line, sizeof(line), f))
                lines.emplace_back(line);
            fclose(f);

            ParseLinesIntoTable(lines, path.c_str());
        }

        void LoadEmbeddedTable(const TranslationEntry* entries, size_t count, const char* langName)
        {
            g_table.clear();
            if (!entries || count == 0) return;
            for (size_t i = 0; i < count; ++i)
            {
                if (entries[i].key && entries[i].val && *entries[i].key && *entries[i].val)
                {
                    g_table[entries[i].key] = Intern(entries[i].val);
                }
            }
            LOG("localisation: loaded %zu embedded translation(s) for %s", count, langName);
        }
    }

    void Discover()
    {
        if (g_discovered) return;
        g_discovered = true;

        g_langs.clear();
        g_langs.push_back({ Intern("en"), Intern("English"), "", nullptr, 0 }); // built in

        const std::string dir = ModuleDir();
        std::vector<std::string> searchDirs;
        if (!dir.empty())
        {
            searchDirs.push_back(dir + "\\Languages");
            searchDirs.push_back(dir + "\\..\\Languages");
        }

        for (const auto& sdir : searchDirs)
        {
            const std::string pattern = sdir + "\\Trinity_*.ini";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::string full = sdir + "\\" + fd.cFileName;
                    std::string code, name;
                    if (!ReadHeader(full, &code, &name)) continue;
                    if (_stricmp(code.c_str(), "en") == 0) continue;

                    bool exists = false;
                    for (const auto& l : g_langs)
                    {
                        if (_stricmp(l.code, code.c_str()) == 0) { exists = true; break; }
                    }
                    if (!exists)
                        g_langs.push_back({ Intern(code), Intern(name), full, nullptr, 0 });
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }

        // Add embedded languages if not present from disk
        for (size_t i = 0; i < kEmbeddedLangsCount; ++i)
        {
            const auto& el = kEmbeddedLangs[i];
            bool exists = false;
            for (const auto& l : g_langs)
            {
                if (_stricmp(l.code, el.code) == 0) { exists = true; break; }
            }
            if (!exists)
            {
                g_langs.push_back({ Intern(el.code), Intern(el.name), "", el.entries, el.count });
            }
        }

        LOG("localisation: discovered %zu language(s) (including built-ins).", g_langs.size() - 1);
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
        if (g_langs[index].embeddedEntries)
        {
            LoadEmbeddedTable(g_langs[index].embeddedEntries, g_langs[index].embeddedCount, g_langs[index].name);
            return;
        }
        if (!g_langs[index].path.empty())
        {
            LoadTable(g_langs[index].path);
            return;
        }
        g_table.clear(); // English
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

    bool NeedsKoreanGlyphs()
    {
        Discover();
        return g_current > 0 && g_current < static_cast<int>(g_langs.size()) &&
               _strnicmp(g_langs[g_current].code, "ko", 2) == 0;
    }

    const char* T(const char* english)
    {
        if (!english || !*english || g_table.empty()) return english;
        const auto it = g_table.find(english);
        return (it == g_table.end()) ? english : it->second;
    }
}
