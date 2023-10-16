#include "log.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "env.h"
#include "error.h"
#include "skynet.h"
#include "socket.h"
#include "timer.h"

FILE *LogOpen(Context *ctx, uint32_t handle) {
  std::string_view logpath = GetEnv("logpath");
  if (logpath == "") return nullptr;
  std::stringstream ss;
  ss << std::setfill('0') << std::setw(8) << std::hex << handle << ".log";
  std::string tmp = std::string(logpath) + "/" + ss.str() + ".log";
  FILE *f = fopen(tmp.c_str(), "ab");
  if (f) {
    uint32_t starttime = Starttime();
    uint64_t currenttime = Now();
    time_t ti = starttime + currenttime / 100;
    SkynetError(ctx->GetHandle(), "Open log file " + tmp);
    fprintf(f, "open time: %u %s", (uint32_t)currenttime, ctime(&ti));
    fflush(f);
  } else {
    SkynetError(ctx->GetHandle(), "Open log file " + tmp + " fail");
  }
  return f;
}

void LogClose(Context *ctx, FILE *f, uint32_t handle) {
  std::stringstream ss;
  ss << std::setfill('0') << std::setw(8) << std::hex << handle << ".log";
  SkynetError(ctx->GetHandle(), "Close log file :" + ss.str());
  fprintf(f, "close time: %u\n", (uint32_t)Now());
  fclose(f);
}

static void LogBlob(FILE *f, uint8_t *buffer, size_t sz) {
  size_t i;
  uint8_t *buf = (uint8_t *)buffer;
  for (i = 0; i != sz; i++) {
    fprintf(f, "%02x", buf[i]);
  }
}

static void LogSocket(FILE *f, struct SkynetSocketMessage *message, size_t sz) {
  fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

  if (message->buffer.empty()) {
    void *buffer = (void *)(message + 1);
    sz -= sizeof(*message);
    void *eol = memchr(buffer, '\0', sz);
    if (eol) {
      sz = (size_t)eol - (size_t)buffer;
    }
    fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
  } else {
    sz = message->ud;
    LogBlob(f, reinterpret_cast<uint8_t*>(message->buffer.data()), sz);
  }
  fprintf(f, "\n");
  fflush(f);
}

void LogOutput(FILE *f, uint32_t source, int type, int session, void *buffer, size_t sz) {
  if (type == kPtypeSocket) {
    LogSocket(f, (struct SkynetSocketMessage *)buffer, sz);
  } else {
    uint32_t ti = (uint32_t)Now();
    fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
    LogBlob(f, (uint8_t*)buffer, sz);
    fprintf(f, "\n");
    fflush(f);
  }
}
