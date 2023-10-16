#pragma once

#include <string>

constexpr int kLogMessageSize = 256;

void SkynetError(uint32_t handle, const std::string msg);
