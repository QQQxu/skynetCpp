#include "command.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "env.h"
#include "error.h"
#include "handle.h"
#include "log.h"
#include "service.h"
#include "skynet.h"
#include "timer.h"

static void ID2Hex(std::string *str, uint32_t id) {
  int i;
  static char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  str[0] = ':';
  for (i = 0; i < 8; i++) {
    str[i + 1] = hex[(id >> ((7 - i) * 4)) & 0xf];
  }
  str[9] = '\0';
}

static std::string_view CmdTimeout(Context &ctx, std::string_view param) {
  std::string::size_type session_ptr;
  long time = std::stol(std::string(param), &session_ptr, 10);
  int session = ctx.Newsession();
  Timeout(ctx.handle_, time, session);
  ctx.result_ = std::to_string(session);
  return ctx.result_;
}

static std::string_view CmdRegister(Context &ctx, std::string_view param) {
  if (param == "") {
    ctx.result_ = std::to_string(ctx.handle_);
    return ctx.result_;
  } else if (param[0] == '.') {
    return HandleNamehandle(ctx.handle_, param);
  }
  std::string args = "Can't register global name " + std::string(param) + " in C";
  SkynetError(ctx.GetHandle(), args);
  return "";
}

static std::string_view CmdQuery(Context &ctx, std::string_view param) {
  if (param[0] == '.') {
    uint32_t handle = HandleFindname(param);
    if (handle) {
      ctx.result_ = std::to_string(handle);
      return ctx.result_;
    }
  }
  return "";
}

/*
param: ".name :00000000(16进制)"
*/
static std::string_view CmdName(Context &ctx, std::string_view param) {
  std::string::size_type pos = param.find_first_of(' ');
  std::string_view name = param.substr(0, pos - 1);
  std::string_view handle = param.substr(pos + 1);
  if (handle[0] != ':') {
    return "";
  }
  uint32_t handle_id = std::stoul(std::string(handle.substr(1)), nullptr, 16);
  if (handle_id == 0) {
    return "";
  }
  if (name[0] == '.') {
    return HandleNamehandle(ctx.handle_, name);
  } else {
    SkynetError(ctx.GetHandle(), "Can't set global name " + std::string(name) + " in C");
  }
  return "";
}

static std::string_view CmdExit(Context &ctx, std::string_view param) {
  HandleExit(ctx, 0);
  return "";
}

static uint32_t ToHandle(Context &ctx, std::string_view param) {
  uint32_t handle = 0;
  if (param[0] == ':') {
    handle = std::stoul(std::string(param.substr(1)), nullptr, 16);
  } else if (param[0] == '.') {
    handle = HandleFindname(param.substr(1));
  } else {
    SkynetError(ctx.GetHandle(), std::string(param));
  }

  return handle;
}

static std::string_view CmdKill(Context &ctx, std::string_view param) {
  uint32_t handle = ToHandle(ctx, param);
  if (handle) {
    HandleExit(ctx, handle);
  }
  return "";
}

/*
param: "contextName contextParm"
*/
static std::string_view CmdLaunch(Context &ctx, std::string_view param) {
  std::string str = std::string(param);
  std::string::size_type pos = str.find_first_of(' ');
  Context *inst = NewContext(str.substr(0, pos - 1), str.substr(pos + 1));
  if (inst == nullptr) {
    return "";
  }
  ID2Hex(&ctx.result_, inst->handle_);
  return ctx.result_;
}

static std::string_view CmdGetEnv(Context &ctx, std::string_view param) { return GetEnv(param); }

static std::string_view CmdSetEnv(Context &ctx, std::string_view param) {
  std::string::size_type pos = param.find_first_of(' ');
  if (pos == std::string::npos) {
    return "";
  }
  std::string_view key = param.substr(0, param.find_first_of(' ') - 1);
  param = param.substr(param.find_first_of(' ') + 1);
  SetEnv(key, param);
  return "";
}

static std::string_view CmdStarttime(Context &ctx, std::string_view param) {
  uint32_t sec = Starttime();
  ctx.result_ = std::to_string(sec);
  return ctx.result_;
}

static std::string_view CmdAbort(Context &ctx, std::string_view param) {
  HandleRetireall();
  return "";
}

static std::string_view CmdMonitor(Context &ctx, std::string_view param) {
  uint32_t handle = 0;
  if (param == "" || param[0] == '\0') {
    uint32_t monitor_exit = GetMoniterExit();
    if (monitor_exit) {
      // return current monitor serivce
      ctx.result_ = std::to_string(monitor_exit);
      return ctx.result_;
    }
    return "";
  } else {
    handle = ToHandle(ctx, param);
  }
  SetMonitorExit(handle);
  return "";
}

static std::string_view CmdStat(Context &ctx, std::string_view param) {
  if (param == "mqlen") {
    int len = ctx.queue_->Length();
    ctx.result_ = std::to_string(len);
  } else if (param == "endless") {
    if (ctx.endless_) {
      ctx.result_ = "1";
      ctx.endless_ = false;
    } else {
      ctx.result_ = "0";
    }
  } else if (param == "cpu") {
    double t = (double)ctx.cpu_cost_ / 1000000.0;  // microsec
    ctx.result_ = std::to_string(t);
  } else if (param == "time") {
    if (ctx.profile_) {
      uint64_t ti = ThreadTime() - ctx.cpu_start_;
      double t = (double)ti / 1000000.0;  // microsec
      ctx.result_ = std::to_string(t);
    } else {
      ctx.result_ = "0";
    }
  } else if (param == "message") {
    ctx.result_ = std::to_string(ctx.message_count_);
  } else {
    ctx.result_ = nullptr;
  }
  return ctx.result_;
}

static std::string_view CmdLogon(Context &ctx, std::string_view param) {
  uint32_t handle = ToHandle(ctx, param);
  if (handle == 0) return "";
  Context *new_ctx = HandleGrab(handle);
  if (new_ctx == nullptr) return "";
  FILE *f = nullptr;
  FILE *lastf = (FILE *)std::atomic_load(&new_ctx->logfile_);
  if (lastf == nullptr) {
    f = LogOpen(&ctx, handle);
    if (f) {
      if (!std::atomic_compare_exchange_weak(&new_ctx->logfile_, 0, (FILE *)f)) {
        // logfile opens in other thread, close this one.
        fclose(f);
      }
    }
  }
  new_ctx->Release();
  return "";
}

static std::string_view CmdLogoff(Context &ctx, std::string_view param) {
  uint32_t handle = ToHandle(ctx, param);
  if (handle == 0) return "";
  Context *new_ctx = HandleGrab(handle);
  if (new_ctx == nullptr) return "";
  FILE *file = (FILE *)std::atomic_load(&new_ctx->logfile_);
  if (file) {
    // logfile may close in other thread
    if (std::atomic_compare_exchange_weak(&new_ctx->logfile_, (FILE **)file, nullptr)) {
      LogClose(&ctx, file, handle);
    }
  }
  new_ctx->Release();
  return "";
}

static std::string_view CmdSignal(Context &ctx, std::string_view param) {
  uint32_t handle = ToHandle(ctx, param);
  if (handle == 0) return "";
  Context *new_ctx = HandleGrab(handle);
  if (new_ctx == nullptr) return "";
  param = param.substr(param.find_first_of(' ') + 1);
  int sig = 0;
  if (param != "") {
    sig = std::stoi(std::string(param), nullptr, 0);
  }
  // NOTICE: the signal function should be thread safe.
  ModuleSignal(*new_ctx->mod_, new_ctx->instance_, sig);
  new_ctx->Release();
  return "";
}

std::map<std::string, command_func> cmd_funcs_ = {
    {"TIMEOUT", CmdTimeout}, {"REG", CmdRegister},        {"QUERY", CmdQuery},   {"NAME", CmdName},
    {"EXIT", CmdExit},       {"KILL", CmdKill},           {"LAUNCH", CmdLaunch}, {"GETENV", CmdGetEnv},
    {"SETENV", CmdSetEnv},   {"STARTTIME", CmdStarttime}, {"ABORT", CmdAbort},   {"MONITOR", CmdMonitor},
    {"STAT", CmdStat},       {"LOGON", CmdLogon},         {"LOGOFF", CmdLogoff}, {"SIGNAL", CmdSignal},
};

std::string_view Command(Context &ctx, std::string cmd, std::string_view parm) {
  auto it = cmd_funcs_.find(cmd);
  if (it != cmd_funcs_.end()) {
    return it->second(ctx, parm);
  }
  return "";
}
