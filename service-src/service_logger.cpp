#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

#include "command.h"
#include "service.h"
#include "skynet.h"
#include "timer.h"

constexpr int kSizeTimeFmt = 250;

struct Logger {
  FILE *handle;
  std::string filename;
  uint32_t starttime;
  int close;
};

extern "C" struct Logger *logger_create(void) {
  struct Logger *logger = new Logger();
  logger->handle = nullptr;
  logger->close = 0;
  return logger;
}

extern "C" void logger_release(struct Logger *logger) {
  if (logger->close) {
    fclose(logger->handle);
  }
  delete logger;
}

static int timestring(struct Logger *inst, char tmp[kSizeTimeFmt]) {
  uint64_t now = Now();
  time_t ti = now / 100 + inst->starttime;
  struct tm info;
  (void)localtime_r(&ti, &info);
  strftime(tmp, kSizeTimeFmt, "%d/%m/%y %H:%M:%S", &info);
  return now % 100;
}

static int logger_cb(struct Context *context, void *ud, int type, int session, uint32_t source, const void *msg,
                     size_t sz) {
  struct Logger *logger = (struct Logger *)ud;
  switch (type) {
    case kPtypeSystem:
      if (!logger->filename.empty()) {
        logger->handle = freopen(logger->filename.c_str(), "a", logger->handle);
      }
      break;
    case kPtypeText:
      if (!logger->filename.empty()) {
        char tmp[kSizeTimeFmt];
        int csec = timestring(logger, tmp);
        fprintf(logger->handle, "%s.%02d ", tmp, csec);
      }
      fprintf(logger->handle, "[:%08x] ", source);
      fwrite(msg, sz, 1, logger->handle);
      fprintf(logger->handle, "\n");
      fflush(logger->handle);
      break;
  }

  return 0;
}

extern "C" int logger_init(struct Logger *logger, struct Context *ctx, const char *parm) {
  std::string_view result = Command(*ctx, "STARTTIME", "");
  logger->starttime = std::stoul(std::string(result));
  if (parm) {
    logger->handle = fopen(parm, "a");
    if (logger->handle == nullptr) {
      return 1;
    }
    logger->filename = std::string(parm);
    logger->close = 1;
  } else {
    logger->handle = stdout;
  }
  if (logger->handle) {
    ServiceCallback(ctx, (void *)logger, logger_cb);
    return 0;
  }
  return 1;
}
