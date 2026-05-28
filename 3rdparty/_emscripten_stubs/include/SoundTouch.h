// Emscripten/WASM stub for SoundTouch.
//
// Purpose: satisfy `#include <SoundTouch.h>` from rpcs3's audio
// post-processing path. The browser POC mutes audio anyway; this stub
// provides just enough class shape for compile to pass. Implementations
// live in libsoundtouch_stub.cpp.

#pragma once

#include <cstdint>

#define SETTING_USE_AA_FILTER       0
#define SETTING_AA_FILTER_LENGTH    1
#define SETTING_NOM_SEQUENCE_LEN    2
#define SETTING_SEQUENCE_MS         3
#define SETTING_NOM_SEEKWINDOW_LEN  4
#define SETTING_SEEKWINDOW_MS       5
#define SETTING_NOM_OVERLAP_LEN     6
#define SETTING_OVERLAP_MS          7
#define SETTING_USE_QUICKSEEK       8

namespace soundtouch
{
class SoundTouch
{
public:
	SoundTouch() {}
	~SoundTouch() {}

	void setSampleRate(uint32_t srate) {}
	void setChannels(uint32_t numChannels) {}
	void setTempo(double newTempo) {}
	void setPitch(double newPitch) {}
	void setRate(double newRate) {}
	void setTempoChange(double newTempo) {}
	void setPitchOctaves(double newPitch) {}
	void setPitchSemiTones(int newPitch) {}
	void setPitchSemiTones(double newPitch) {}
	void putSamples(const float* samples, uint32_t numSamples) {}
	uint32_t receiveSamples(float* output, uint32_t maxSamples) { return 0; }
	uint32_t receiveSamples(uint32_t maxSamples) { return 0; }
	float* bufBegin() { return nullptr; }
	uint32_t numSamples() const { return 0; }
	void clear() {}
	void flush() {}
	// Per-setting API rpcs3 uses; values like SETTING_USE_QUICKSEEK,
	// SETTING_SEQUENCE_MS, etc. Return 0 / true on stub.
	bool setSetting(int settingId, int value) { return true; }
	int  getSetting(int settingId) const { return 0; }
	double getInputOutputSampleRatio() const { return 1.0; }
};
}
