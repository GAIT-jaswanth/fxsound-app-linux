/*
 * Linux replacement for the Windows registry tree HKCU\Software\DFX that the
 * DSP uses for its "last used" settings (see dsp_support.cpp).
 */
#pragma once

/* Equivalent of RegDeleteTree(HKEY_CURRENT_USER, L"Software\\DFX"): removes
 * every stored value, both from disk and from the in-memory cache. */
void fxRegistryClearAll();
