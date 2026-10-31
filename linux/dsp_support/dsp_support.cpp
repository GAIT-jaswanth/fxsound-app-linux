/*
FxSound - Linux support layer for the DfxDsp engine.

The Windows build links DfxDsp against a few helpers from audiopassthru/
(file, string, registry) that are written directly against Win32. This file
provides Linux implementations of exactly the helpers the DSP uses, with the
same signatures and semantics.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "windows.h"
#include "codedefs.h"
#include "slout.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

// ---------------------------------------------------------------------------
// UTF-8 <-> wchar_t (UTF-32 on Linux)
// ---------------------------------------------------------------------------
static std::string toUtf8(const wchar_t* w)
{
    std::string out;
    for (; w && *w; ++w) {
        uint32_t c = (uint32_t)*w;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static std::wstring fromUtf8(const char* s)
{
    std::wstring out;
    const unsigned char* p = (const unsigned char*)s;
    while (p && *p) {
        uint32_t c = *p++;
        int extra = 0;
        if (c >= 0xF0) { c &= 0x07; extra = 3; }
        else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
        else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i) c = (c << 6) | (*p++ & 0x3F);
        out += (wchar_t)c;
    }
    return out;
}

static std::string pathToUtf8(const wchar_t* w)
{
    std::string s = toUtf8(w);
    for (auto& ch : s) if (ch == '\\') ch = '/';
    return s;
}

// ---------------------------------------------------------------------------
// FILE helpers
// ---------------------------------------------------------------------------
int fileExist_Wide(wchar_t* wcp_name, int* ip_exist)
{
    if (wcp_name == NULL) return NOT_OKAY;
    struct stat st;
    *ip_exist = (stat(pathToUtf8(wcp_name).c_str(), &st) == 0) ? IS_TRUE : IS_FALSE;
    return OKAY;
}

FILE* fileOpen_Wide(wchar_t* wcp_name, wchar_t* wcp_mode, CSlout* /*hp_slout*/)
{
    if (wcp_name == NULL) return NULL;
    return _wfopen(wcp_name, wcp_mode);
}

// ---------------------------------------------------------------------------
// pstr helpers
// ---------------------------------------------------------------------------
int pstrCalcLocationOfStrInStr_Wide(wchar_t* wcp_haystack, wchar_t* wcp_needle,
                                    int i_start_search_index, int* ip_location, int* ip_found)
{
    if (wcp_haystack == NULL || wcp_needle == NULL || i_start_search_index < 0) return NOT_OKAY;
    *ip_found = IS_FALSE;
    *ip_location = -1;
    wchar_t* idx = wcsstr(wcp_haystack + i_start_search_index, wcp_needle);
    if (idx) { *ip_location = (int)(idx - wcp_haystack); *ip_found = IS_TRUE; }
    return OKAY;
}

int pstrCovertWideCharStringToUTF8String_WithAlloc(wchar_t* wcp_in, char** cpp_out, int* ip_len)
{
    if (wcp_in == NULL || cpp_out == NULL) return NOT_OKAY;
    std::string s = toUtf8(wcp_in);
    *cpp_out = (char*)calloc(s.size() + 1, 1);
    if (!*cpp_out) return NOT_OKAY;
    memcpy(*cpp_out, s.c_str(), s.size());
    if (ip_len) *ip_len = (int)s.size();
    return OKAY;
}

int pstrCovertUTF8StringToWideCharString_WithAlloc(char* cp_in, wchar_t** wcpp_out, int* ip_len)
{
    if (cp_in == NULL || wcpp_out == NULL) return NOT_OKAY;
    // Preset files written on Windows have CRLF line endings; drop the CR so
    // names don't end in a stray '\r'.
    std::string in(cp_in);
    size_t cr;
    while ((cr = in.find('\r')) != std::string::npos) in.erase(cr, 1);
    std::wstring w = fromUtf8(in.c_str());
    *wcpp_out = (wchar_t*)calloc(w.size() + 1, sizeof(wchar_t));
    if (!*wcpp_out) return NOT_OKAY;
    wmemcpy(*wcpp_out, w.c_str(), w.size());
    if (ip_len) *ip_len = (int)w.size();
    return OKAY;
}

// ---------------------------------------------------------------------------
// Registry replacement.
// Windows stores EQ/session values under HKCU\Software\DFX\... . On Linux the
// same key/value pairs live in $XDG_CONFIG_HOME/FxSound/dsp_registry.txt
// (one "key<TAB>value" pair per line, UTF-8).
// ---------------------------------------------------------------------------
namespace {
std::mutex g_reg_mutex;
std::map<std::wstring, std::wstring> g_reg;
bool g_reg_loaded = false;

std::string configDir()
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? xdg : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config";
    return base + "/FxSound";   // same folder the GUI uses for settings, presets and logs
}

std::string regFile() { return configDir() + "/dsp_registry.txt"; }

std::wstring regKey(int i_key_class, const wchar_t* name)
{
    return std::to_wstring(i_key_class) + L":" + name;
}

void regLoadLocked()
{
    if (g_reg_loaded) return;
    g_reg_loaded = true;
    FILE* f = fopen(regFile().c_str(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        g_reg[fromUtf8(line)] = fromUtf8(tab + 1);
    }
    fclose(f);
}

void regSaveLocked()
{
    std::string dir = configDir();
    std::string parent = dir.substr(0, dir.rfind('/'));
    mkdir(parent.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::string tmp = regFile() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    for (auto& kv : g_reg)
        fprintf(f, "%s\t%s\n", toUtf8(kv.first.c_str()).c_str(), toUtf8(kv.second.c_str()).c_str());
    fclose(f);
    rename(tmp.c_str(), regFile().c_str());
}
} // namespace

int regCreateKey_Wide(int i_key_class, wchar_t* wcp_keyname, wchar_t* wcp_data)
{
    if (wcp_keyname == NULL || wcp_data == NULL) return NOT_OKAY;
    std::lock_guard<std::mutex> lock(g_reg_mutex);
    regLoadLocked();
    auto key = regKey(i_key_class, wcp_keyname);
    auto it = g_reg.find(key);
    if (it != g_reg.end() && it->second == wcp_data) return OKAY;
    g_reg[key] = wcp_data;
    regSaveLocked();
    return OKAY;
}

void fxRegistryClearAll()
{
    std::lock_guard<std::mutex> lock(g_reg_mutex);
    g_reg.clear();
    g_reg_loaded = true;   // nothing left to load
    remove(regFile().c_str());
}

int regReadKey_Wide(int i_key_class, wchar_t* wcp_keyname, int* ip_key_exists_flag,
                    wchar_t* wcp_data, unsigned long ul_buffer_size)
{
    *ip_key_exists_flag = IS_FALSE;
    if (wcp_keyname == NULL) return NOT_OKAY;
    std::lock_guard<std::mutex> lock(g_reg_mutex);
    regLoadLocked();
    auto it = g_reg.find(regKey(i_key_class, wcp_keyname));
    if (it == g_reg.end()) return OKAY;
    size_t max_chars = ul_buffer_size / sizeof(wchar_t);
    if (max_chars == 0) return OKAY;
    wcsncpy(wcp_data, it->second.c_str(), max_chars - 1);
    wcp_data[max_chars - 1] = 0;
    *ip_key_exists_flag = IS_TRUE;
    return OKAY;
}

// ---------------------------------------------------------------------------
// 24-bit packed <-> float. Rewritten with fixed-width ints: the original uses
// `long`, which is 64-bit on Linux and breaks the 3-byte memcpy + shift trick.
// ---------------------------------------------------------------------------
int pwav24BitToFloat(char* cp_24_bit, float* rp_float, int i_length, int i_stereo_flag)
{
    if (i_stereo_flag) i_length *= 2;
    for (int i = 0; i < i_length; i++) {
        const unsigned char* b = (const unsigned char*)cp_24_bit + i * 3;
        int32_t v = (int32_t)(((uint32_t)b[0] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 24));
        rp_float[i] = (float)v * (1.0f / 2147483648.0f);
    }
    return OKAY;
}

int pwavFloatTo24Bit(float* rp_float, char* cp_24_bit, int i_length, int i_stereo_flag)
{
    if (i_stereo_flag) i_length *= 2;
    for (int i = 0; i < i_length; i++) {
        float f = rp_float[i];
        int32_t v;
        if (f >= 1.0f) v = 2147483647;
        else if (f <= -1.0f) v = -2147483647;
        else v = (int32_t)(f * 2147483648.0f);
        unsigned char* b = (unsigned char*)cp_24_bit + i * 3;
        b[0] = (unsigned char)((v >> 8) & 0xFF);
        b[1] = (unsigned char)((v >> 16) & 0xFF);
        b[2] = (unsigned char)((v >> 24) & 0xFF);
    }
    return OKAY;
}

// ---------------------------------------------------------------------------
// Legacy DSP-card hardware access. There's no such hardware on Linux (nor on
// any modern Windows install); fail cleanly like the Windows build does when
// no card is present.
// ---------------------------------------------------------------------------
extern "C" int comHrdEepromReadUlong(short unsigned, short unsigned, unsigned long* ulp) { if (ulp) *ulp = 0; return NOT_OKAY_NO_BREAK; }
extern "C" int comHrdEepromWriteUlong(short unsigned, short unsigned, unsigned long) { return NOT_OKAY_NO_BREAK; }
extern "C" int comHrdwrGetChar(int, long* lp) { if (lp) *lp = 0; return NOT_OKAY_NO_BREAK; }

// ---------------------------------------------------------------------------
// mth string helper (the original lives in a WIN32-only block of MthUtil.cpp)
// ---------------------------------------------------------------------------
int mthIsLong_Wide(wchar_t* wcp_string, int* ip_is_long)
{
    *ip_is_long = IS_FALSE;
    if (wcp_string == NULL) return NOT_OKAY;
    int length = (int)wcslen(wcp_string);
    for (int i = 0; i < length; i++) {
        wchar_t c = wcp_string[i];
        if ((c < L'0' || c > L'9') && !(i == 0 && c == L'-')) return OKAY;
    }
    if (length > 0) *ip_is_long = IS_TRUE;
    return OKAY;
}
