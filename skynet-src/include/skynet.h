#pragma once

#include <string_view>

#include "service.h"

constexpr int kPtypeText = 0;
constexpr int kPtypeResponse = 1;
constexpr int kPtypeMulticast = 2;
constexpr int kPtypeClient = 3;
constexpr int kPtypeSystem = 4;
constexpr int kPtypeHarbor = 5;
constexpr int kPtypeSocket = 6;
// read lualib/skynet.lua examples/simplemonitor.lua
constexpr int kPtypeError = 7;
// read lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua
constexpr int kPtypeReservedOueue = 8;
constexpr int kPtypeReservedDebug = 9;
constexpr int kPtypeReservedLua = 10;
constexpr int kPtypeReservedSnax = 11;

constexpr int kPtypeTagDontcopy = 0x10000;
constexpr int kPtypeTagAllocsession = 0x20000;

uint32_t SkynetQueryname(Context *ctx, std::string_view name);
