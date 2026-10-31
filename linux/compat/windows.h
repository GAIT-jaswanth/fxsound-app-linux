/* Minimal Win32 compatibility shim for building FxSound's DSP on Linux. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int INT;
typedef float FLOAT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef wchar_t TCHAR;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef wchar_t *LPTSTR;
typedef const wchar_t *LPCTSTR;
typedef void *LPVOID;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HKEY;
typedef void *HMODULE;
typedef long LRESULT;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;
typedef DWORD *LPDWORD;
typedef BYTE *LPBYTE;
typedef long HRESULT;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORDLONG;
typedef uintptr_t DWORD_PTR;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
#define __int64 int64_t
typedef DWORD REGSAM;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define WINAPI
#define CALLBACK
#define APIENTRY
#define __stdcall
#define __cdecl
#define _cdecl
#define __declspec(x)
#define _T(x) L##x
#define TEXT(x) L##x
#define _TEXT(x) L##x
#define S_OK 0L
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define ERROR_SUCCESS 0L
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#define _stricmp strcasecmp
#define stricmp strcasecmp
#define _strnicmp strncasecmp
#define strnicmp strncasecmp
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define wcsicmp wcscasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _snwprintf swprintf
#define _wtoi(s) ((int)wcstol((s), NULL, 10))
#define _wtof(s) wcstod((s), NULL)
#define _isnan isnan
#define _finite isfinite

static inline void Sleep(DWORD ms) { struct timespec t = { (time_t)(ms/1000), (long)(ms%1000)*1000000L }; nanosleep(&t, NULL); }
#include <time.h>
static inline DWORD GetTickCount(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (DWORD)(t.tv_sec*1000 + t.tv_nsec/1000000); }
static inline DWORD timeGetTime(void) { return GetTickCount(); }
static inline void OutputDebugStringA(const char*) {}
static inline void OutputDebugStringW(const wchar_t*) {}
#define OutputDebugString OutputDebugStringW
#define MessageBox MessageBoxW

#define MB_OK 0
#define MB_ICONERROR 0
static inline int MessageBoxA(HWND, const char* t, const char* c, UINT) { fprintf(stderr, "[fxsound] %s: %s\n", c?c:"", t?t:""); return 0; }
static inline int MessageBoxW(HWND, const wchar_t* t, const wchar_t* c, UINT) { fprintf(stderr, "[fxsound] %ls: %ls\n", c?c:L"", t?t:L""); return 0; }
static inline BOOL IsDebuggerPresent(void) { return FALSE; }
static inline void DebugBreak(void) {}

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;


typedef void *LPSECURITY_ATTRIBUTES;
#define _wtol(s) wcstol((s), NULL, 10)
#define _wtoi64(s) wcstoll((s), NULL, 10)

/* Wide-path file helpers, implemented in win_compat.cpp (UTF-8 on disk). */
#ifdef __cplusplus
extern "C" {
#endif
FILE* _wfopen(const wchar_t* path, const wchar_t* mode);
int _wfopen_s(FILE** fp, const wchar_t* path, const wchar_t* mode);
int _wremove(const wchar_t* path);
int _wmkdir(const wchar_t* path);
int _waccess(const wchar_t* path, int mode);
#ifdef __cplusplus
}
#endif

/* No DLL loading on Linux: callers already handle a NULL module. */
static inline HMODULE LoadLibraryW(const wchar_t*) { return NULL; }
static inline HMODULE LoadLibraryA(const char*) { return NULL; }
#define LoadLibrary LoadLibraryW
static inline void* GetProcAddress(HMODULE, const char*) { return NULL; }
static inline BOOL FreeLibrary(HMODULE) { return TRUE; }

/*
 * Windows wide-char stdio semantics.
 * MSVC: in wide printf/scanf "%s" means wchar_t*, "%S"/"%hs" means char*.
 * glibc: "%s" means char*, "%ls" means wchar_t*.
 * Legacy code also uses MSVC's size-less swprintf and byte-oriented wide
 * text files. These wrappers keep the original Windows behavior.
 */
#ifdef __cplusplus
#include <cstdarg>
#include <cwchar>
#include <cstdio>
#include <string>
extern "C" {
#endif
int fx_vswprintf_win(wchar_t* buf, size_t n, const wchar_t* fmt, va_list ap);
int fx_swprintf_n(wchar_t* buf, size_t n, const wchar_t* fmt, ...);
int fx_fwprintf(FILE* f, const wchar_t* fmt, ...);
int fx_swscanf(const wchar_t* s, const wchar_t* fmt, ...);
wchar_t* fx_fgetws(wchar_t* buf, int n, FILE* f);
int fx_fputws(const wchar_t* s, FILE* f);
#ifdef __cplusplus
}
static inline int fx_swprintf(wchar_t* buf, const wchar_t* fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = fx_vswprintf_win(buf, 0x7fffffff / sizeof(wchar_t), fmt, ap); va_end(ap); return r; }
static inline int fx_swprintf(wchar_t* buf, size_t n, const wchar_t* fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = fx_vswprintf_win(buf, n, fmt, ap); va_end(ap); return r; }
#define swprintf fx_swprintf
#define _swprintf fx_swprintf
#define swprintf_s fx_swprintf
#endif
#define fwprintf fx_fwprintf
#define swscanf fx_swscanf
#define fgetws fx_fgetws
#define fputws fx_fputws
#ifdef __cplusplus
extern "C"
#endif
int fx_sprintf(char* buf, const char* fmt, ...);
#define sprintf fx_sprintf
