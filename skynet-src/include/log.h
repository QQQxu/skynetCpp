#pragma once

#include <cstdint>
#include <cstdio>

#include "service.h"

FILE *LogOpen(Context *ctx, uint32_t handle);
void LogClose(Context *ctx, FILE *f, uint32_t handle);
void LogOutput(FILE *f, uint32_t source, int type, int session, void *buffer, size_t sz);
