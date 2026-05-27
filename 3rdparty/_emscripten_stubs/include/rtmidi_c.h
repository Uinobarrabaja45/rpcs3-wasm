// Emscripten/WASM stub for rtmidi C API. The browser POC has no MIDI
// device access (Web MIDI is a separate effort). Implementations live
// in rtmidi_stub.cpp; for now just satisfy compile of the few rpcs3
// files that #include this.

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RtMidiWrapper {
	void* ptr;
	void* data;
	bool  ok;
	const char* msg;
} RtMidiWrapper;

typedef RtMidiWrapper* RtMidiPtr;
typedef RtMidiWrapper* RtMidiInPtr;
typedef RtMidiWrapper* RtMidiOutPtr;

typedef enum {
	RT_MIDI_API_UNSPECIFIED = 0,
} RtMidiApi;

RtMidiInPtr  rtmidi_in_create_default(void);
RtMidiOutPtr rtmidi_out_create_default(void);
void rtmidi_in_free(RtMidiInPtr device);
void rtmidi_out_free(RtMidiOutPtr device);
unsigned int rtmidi_get_port_count(RtMidiPtr device);
const char* rtmidi_get_port_name(RtMidiPtr device, unsigned int portNumber);
void rtmidi_open_port(RtMidiPtr device, unsigned int portNumber, const char* portName);
void rtmidi_close_port(RtMidiPtr device);
double rtmidi_in_get_message(RtMidiInPtr device, unsigned char* message, size_t* size);
int  rtmidi_out_send_message(RtMidiOutPtr device, const unsigned char* message, int length);

#ifdef __cplusplus
}
#endif
