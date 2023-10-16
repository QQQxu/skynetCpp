#pragma once

#include <string_view>

std::string_view GetEnv(std::string_view key);
void SetEnv(std::string_view key, std::string_view value);

void EnvInit();
