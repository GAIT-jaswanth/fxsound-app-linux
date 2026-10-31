/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Linux (PipeWire) version of audiopassthru/include/AudioPassthru.h.
 *
 * The public API is identical to the Windows header so the GUI code compiles
 * unchanged. Differences in SoundDevice: there is no IMMDevice pointer, and
 * device IDs (pwszID) are PipeWire node names (e.g.
 * "alsa_output.pci-0000_00_1f.3.analog-stereo"), which are stable across
 * reboots like Windows endpoint GUIDs.
 *
 * Instead of a kernel driver, the "FxSound Audio Enhancer" virtual output is a
 * PipeWire sink node created by this process. Apps play into it, the DSP
 * processes the audio, and it is played on the selected real device.
 */
#ifndef _AUDIOPASSTHRU_H_
#define _AUDIOPASSTHRU_H_

#include <string>
#include <vector>
#include "DfxDsp.h"

struct SoundDevice {
	bool isCaptureDevice = false;
	bool isPlaybackDevice = false;
	bool isTargetedRealPlaybackDevice = false;
	bool isRealDevice = false;
	bool isDFXDevice = false;
	bool isUserSelectedPlaybackDevice = false;
	bool isDefaultDevice = false;
	bool isActive = false;

	std::wstring pwszID;                // PipeWire node.name
	std::wstring pwszIDRealDevices;
	std::wstring deviceFriendlyName;    // node.description, e.g. "Built-in Audio Analog Stereo"
	std::wstring deviceDescription;     // node.nick or a generic kind, e.g. "Speakers"
	std::wstring deviceFriendlyNameRealDevices;
	std::wstring deviceDescriptionRealDevices;
	std::wstring deviceFormFactor;      // device.form-factor when PipeWire knows it
	int deviceNumChannel = 2;
};

class AudioPassthruCallback
{
public:
	virtual ~AudioPassthruCallback() = default;
	virtual void onSoundDeviceChange(bool processing) = 0;
};

class AudioPassthruPrivate;
class AudioPassthru
{
public:
	AudioPassthru();
	~AudioPassthru();
	int init();
	void mute(bool mute);
	std::vector<SoundDevice> getSoundDevices(bool active_devices = true);
	int setBufferLength(int i_buffer_length_msecs);
	int processTimer();
	void setDspProcessingModule(DfxDsp* pDspProcessingModule);
	void setAsPlaybackDevice(const SoundDevice sound_device);
	void registerCallback(AudioPassthruCallback *callback);
	bool isPlaybackDeviceAvailable();
	bool checkDeviceChanges();
	void restoreDefaultPlaybackDevice();

private:
	AudioPassthruPrivate *data_;
};

/* Name of the virtual sink, as seen by other applications. The GUI looks for
 * this string to decide that the FxSound device is present. */
#define FXSOUND_SINK_DESCRIPTION L"FxSound Audio Enhancer"
#define FXSOUND_SINK_NODE_NAME   "fxsound_audio_enhancer"

#endif
