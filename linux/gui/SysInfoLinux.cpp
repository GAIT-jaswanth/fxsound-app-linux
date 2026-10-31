/*
FxSound - Linux implementation of fxsound/Source/Utils/SysInfo (the Windows
version uses WASAPI and the Service Control Manager).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "SysInfo.h"
#include "AudioPassthru.h"

void SysInfo::enumAudioOutputs(StringArray& audioOutputNames)
{
    AudioPassthru passthru;
    if (passthru.init() != 0) return;
    for (auto& d : passthru.getSoundDevices())
        if (d.isRealDevice) audioOutputNames.add(String(d.deviceFriendlyName.c_str()));
}

bool SysInfo::isServiceRunning(const wchar_t* service_name)
{
    // Ask systemd (user units first, then system units).
    String name(service_name);
    if (name.isEmpty()) return false;
    for (auto scope : { "--user", "--system" }) {
        ChildProcess p;
        if (p.start(StringArray { "systemctl", scope, "is-active", "--quiet", name }, 0)
            && p.waitForProcessToFinish(2000) && p.getExitCode() == 0)
            return true;
    }
    return false;
}

bool SysInfo::canSupportHotkeys()
{
    // There is no portable global-hotkey API on Linux (Wayland forbids global
    // key grabs). Returning false hides the hotkey settings in the UI.
    // Users can bind desktop shortcuts to `FxSound --toggle` etc. instead.
    return false;
}

bool SysInfo::isRemoteSession()
{
    return false;
}
