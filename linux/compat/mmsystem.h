#pragma once
#include "windows.h"
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
    WORD wFormatTag; WORD nChannels; DWORD nSamplesPerSec; DWORD nAvgBytesPerSec;
    WORD nBlockAlign; WORD wBitsPerSample; WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
typedef struct pcmwaveformat_tag { struct { WORD wFormatTag; WORD nChannels; DWORD nSamplesPerSec; DWORD nAvgBytesPerSec; WORD nBlockAlign; } wf; WORD wBitsPerSample; } PCMWAVEFORMAT;
#pragma pack(pop)
#endif
