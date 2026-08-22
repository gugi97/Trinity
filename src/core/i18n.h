#pragma once
#include <cstddef>

// Menu translation.
//
// Keys are the ENGLISH STRINGS THEMSELVES, not invented identifiers. That one
// choice does most of the work here: a translation file only needs the lines
// someone actually translated, anything missing falls through to English on its
// own, and adding a language never touches the code. It also means the widgets
// can translate their own labels, so the ~350 call sites across the menu stay
// exactly as they are.
//
// Files live next to Trinity.asi:
//     Languages\Trinity_id.ini      [Language] Name=Bahasa Indonesia  Code=id
//     Languages\Trinity_zh.ini      [Language] Name=简体中文          Code=zh
//
// Lines are `English text=translated text`, split on the FIRST '=' so English
// containing '=' still works. Lines starting with ';' or '#' are comments.
namespace trinity::i18n
{
    // Scan the Languages folder. English is always index 0 and needs no file.
    void Discover();

    int         LanguageCount();
    const char* LanguageName(int index);   // display name, e.g. "Bahasa Indonesia"
    const char* LanguageCode(int index);   // "en", "id", "zh"

    // Index of the active language, and switching to another (loads on demand).
    int  CurrentLanguage();
    void SetLanguage(int index);

    // Select by code, for restoring the saved setting. Falls back to English
    // when that language is not installed.
    void SetLanguageByCode(const char* code);

    // Does the active language need CJK glyphs? The font atlas is built once at
    // startup, so this is read before the fonts load.
    bool NeedsCjkGlyphs();

    // Translate. Returns `english` unchanged when there is no entry - so an
    // untranslated string is merely untranslated, never blank.
    // The returned pointer stays valid until the next SetLanguage().
    const char* T(const char* english);
}
