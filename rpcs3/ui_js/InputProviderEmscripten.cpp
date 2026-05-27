#include "InputProviderEmscripten.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace
{
	std::unordered_set<std::string> g_pressed;
}

void CInputProviderEmscripten::OnKeyDown(const char* code)
{
	if (!code) return;
	g_pressed.insert(code);
}

void CInputProviderEmscripten::OnKeyUp(const char* code)
{
	if (!code) return;
	g_pressed.erase(code);
}

bool CInputProviderEmscripten::IsKeyDown(const char* code) const
{
	if (!code) return false;
	return g_pressed.find(code) != g_pressed.end();
}
