#pragma once

#include <emscripten/html5.h>
#include <string>

// Emscripten keyboard → DualShock3 pad input bridge. rpcs3 already has a
// keyboard handler under Input/keyboard_pad_handler.cpp — for the browser
// we use this thinner provider that just forwards EmscriptenKeyboardEvent
// codes upstream. The actual mapping (KeyA → PS3 Cross, etc.) is set up in
// Main.cpp::initVmWebGPU at boot time.
class CInputProviderEmscripten
{
public:
	CInputProviderEmscripten() = default;
	~CInputProviderEmscripten() = default;

	void OnKeyDown(const char* code);
	void OnKeyUp(const char* code);

	// Whether a given DOM KeyboardEvent.code is currently pressed.
	bool IsKeyDown(const char* code) const;
};
