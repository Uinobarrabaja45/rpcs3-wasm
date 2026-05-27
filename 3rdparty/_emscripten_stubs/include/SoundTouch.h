// Emscripten/WASM stub for SoundTouch.
//
// Purpose: satisfy `#include <SoundTouch.h>` from rpcs3's audio
// post-processing path. The browser POC mutes audio anyway; this stub
// provides just enough class shape for compile to pass. Implementations
// live in libsoundtouch_stub.cpp.

#pragma once

#include <cstdint>

namespace soundtouch
{
class SoundTouch
{
public:
	SoundTouch();
	~SoundTouch();

	void setSampleRate(uint32_t srate);
	void setChannels(uint32_t numChannels);
	void setTempo(double newTempo);
	void setPitch(double newPitch);
	void setRate(double newRate);
	void setTempoChange(double newTempo);
	void setPitchOctaves(double newPitch);
	void setPitchSemiTones(int newPitch);
	void setPitchSemiTones(double newPitch);
	void putSamples(const float* samples, uint32_t numSamples);
	uint32_t receiveSamples(float* output, uint32_t maxSamples);
	uint32_t numSamples() const;
	void clear();
	void flush();
};
}
