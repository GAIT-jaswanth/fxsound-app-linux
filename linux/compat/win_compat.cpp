// Wide-character path helpers for the Linux build. Windows code passes
// wchar_t paths everywhere; Linux filesystems take UTF-8 bytes.
#include "windows.h"
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string narrow(const wchar_t* w)
{
    std::string out;
    if (!w) return out;
    for (; *w; ++w) {
        uint32_t c = (uint32_t)*w;
        if (c == L'\\') c = L'/';               // tolerate Windows separators
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static std::string narrowMode(const wchar_t* m)
{
    std::string out;
    for (; m && *m; ++m) if (*m != L',' && *m != L' ') { if (*m == L'c' || *m == L'n' || *m == L'S' || *m == L'R' || *m == L'T' || *m == L'D') continue; out += (char)*m; }
    // drop "ccs=..." encodings
    auto p = out.find("=");
    if (p != std::string::npos) out = out.substr(0, out.find_last_not_of("=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-", p) + 1);
    return out;
}

extern "C" FILE* _wfopen(const wchar_t* path, const wchar_t* mode)
{
    return fopen(narrow(path).c_str(), narrowMode(mode).c_str());
}

extern "C" int _wfopen_s(FILE** fp, const wchar_t* path, const wchar_t* mode)
{
    *fp = _wfopen(path, mode);
    return *fp ? 0 : 1;
}

extern "C" int _wremove(const wchar_t* path) { return remove(narrow(path).c_str()); }
extern "C" int _wmkdir(const wchar_t* path) { return mkdir(narrow(path).c_str(), 0755); }
extern "C" int _waccess(const wchar_t* path, int mode) { return access(narrow(path).c_str(), mode); }

// ---------------------------------------------------------------------------
// Windows wide-format translation
// ---------------------------------------------------------------------------
static std::wstring winFmtToGlibc(const wchar_t* f, bool scanf_mode)
{
    std::wstring out;
    while (*f) {
        if (*f != L'%') { out += *f++; continue; }
        out += *f++;
        if (*f == L'%') { out += *f++; continue; }
        // flags / width / precision / '*'
        while (*f && wcschr(L"-+ #0*123456789.", *f)) out += *f++;
        // length modifiers
        bool h = false, l = false;
        if (f[0] == L'I' && f[1] == L'6' && f[2] == L'4') { out += L"ll"; f += 3; }
        else if (f[0] == L'I' && f[1] == L'3' && f[2] == L'2') { f += 3; }
        else if (*f == L'I') { out += L'z'; f++; }
        while (*f && wcschr(L"hlLqjzt", *f)) {
            if (*f == L'h' && (f[1] == L's' || f[1] == L'c' || f[1] == L'S' || f[1] == L'C' || f[1] == L'[')) { h = true; f++; continue; }
            // The DSP is compiled with a 32-bit `long` (see dsp_prelude.h), so a
            // single 'l' on an integer conversion means 32 bits: drop it. "ll" stays.
            if (*f == L'l' && f[1] != L'l' && f[1] && wcschr(L"diouxX", f[1])) { f++; continue; }
            if (*f == L'l' && f[1] == L'l') { out += *f++; out += *f++; continue; }
            if (*f == L'l') l = true;
            out += *f++;
        }
        wchar_t c = *f;
        if (!c) break;
        if (c == L's' || c == L'c' || c == L'[') {
            if (!h && !l) out += L'l';               // MSVC wide default = wchar_t
        } else if (c == L'S' || c == L'C') {
            c = (c == L'S') ? L's' : L'c';           // MSVC wide %S = narrow
        }
        out += c; f++;
        if (c == L'[' && scanf_mode) {
            if (*f == L'^') out += *f++;
            if (*f == L']') out += *f++;
            while (*f && *f != L']') out += *f++;
            if (*f) out += *f++;
        }
    }
    return out;
}

extern "C" int fx_vswprintf_win(wchar_t* buf, size_t n, const wchar_t* fmt, va_list ap)
{
    std::wstring f = winFmtToGlibc(fmt, false);
    return vswprintf(buf, n, f.c_str(), ap);
}

extern "C" int fx_swprintf_n(wchar_t* buf, size_t n, const wchar_t* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = fx_vswprintf_win(buf, n, fmt, ap);
    va_end(ap); return r;
}

// Windows writes wide text to a non-ccs stream as single bytes; do the same
// (values > 0xFF are emitted as UTF-8 so nothing is lost).
static int putWideAsBytes(const wchar_t* s, FILE* f)
{
    int n = 0;
    for (; *s; ++s, ++n) {
        uint32_t c = (uint32_t)*s;
        if (c < 0x100) fputc((int)c, f);
        else { wchar_t one[2] = { (wchar_t)c, 0 }; fputs(narrow(one).c_str(), f); }
    }
    return n;
}

extern "C" int fx_fwprintf(FILE* f, const wchar_t* fmt, ...)
{
    std::wstring wf = winFmtToGlibc(fmt, false);
    va_list ap; va_start(ap, fmt);
    std::wstring buf(8192, L'\0');
    int r = vswprintf(&buf[0], buf.size(), wf.c_str(), ap);
    va_end(ap);
    if (r < 0) return r;
    buf.resize(r);
    return putWideAsBytes(buf.c_str(), f);
}

extern "C" int fx_fputws(const wchar_t* s, FILE* f) { putWideAsBytes(s, f); return 0; }

extern "C" int fx_swscanf(const wchar_t* s, const wchar_t* fmt, ...)
{
    std::wstring wf = winFmtToGlibc(fmt, true);
    va_list ap; va_start(ap, fmt);
    int r = vswscanf(s, wf.c_str(), ap);
    va_end(ap); return r;
}

// Byte-oriented line read (Windows text mode: CRLF -> LF, each byte -> one wchar).
extern "C" wchar_t* fx_fgetws(wchar_t* buf, int n, FILE* f)
{
    if (n <= 0) return NULL;
    int i = 0, ch = EOF;
    while (i < n - 1 && (ch = fgetc(f)) != EOF) {
        if (ch == '\r') { int nx = fgetc(f); if (nx != '\n' && nx != EOF) ungetc(nx, f); ch = '\n'; }
        buf[i++] = (wchar_t)(unsigned char)ch;
        if (ch == '\n') break;
    }
    if (i == 0 && ch == EOF) return NULL;
    buf[i] = 0;
    return buf;
}

// Narrow sprintf used by DSP debug messages. Narrow %s means char* on both
// platforms; only the 32-bit-long rule needs translating.
extern "C" int fx_sprintf(char* buf, const char* fmt, ...)
{
    std::string nf;
    for (const char* p = fmt; *p; ) {
        if (*p != '%') { nf += *p++; continue; }
        nf += *p++;
        if (*p == '%') { nf += *p++; continue; }
        while (*p && strchr("-+ #0*123456789.", *p)) nf += *p++;
        if (p[0] == 'l' && p[1] != 'l' && p[1] && strchr("diouxX", p[1])) p++;
        while (*p && strchr("hlLqjzt", *p)) nf += *p++;
        if (*p) nf += *p++;
    }
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, nf.c_str(), ap);
    va_end(ap); return r;
}
