#pragma once

#include <string>

bool SetAutostart(bool enabled, std::wstring* error);
bool IsAutostartEnabled();
