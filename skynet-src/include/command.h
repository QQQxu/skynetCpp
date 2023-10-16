#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "service.h"

static std::string_view CmdTimeout(Context &ctx, std::string_view param);

static std::string_view CmdRegister(Context &ctx, std::string_view param);

static std::string_view CmdQuery(Context &ctx, std::string_view param);

static std::string_view CmdName(Context &ctx, std::string_view param);

static std::string_view CmdExit(Context &ctx, std::string_view param);

static std::string_view CmdKill(Context &ctx, std::string_view param);

static std::string_view CmdLaunch(Context &ctx, std::string_view param);

static std::string_view CmdGetEnv(Context &ctx, std::string_view param);

static std::string_view CmdSetEnv(Context &ctx, std::string_view param);

static std::string_view CmdStarttime(Context &ctx, std::string_view param);

static std::string_view CmdAbort(Context &ctx, std::string_view param);

static std::string_view CmdMonitor(Context &ctx, std::string_view param);

static std::string_view CmdStat(Context &ctx, std::string_view param);

static std::string_view CmdLogon(Context &ctx, std::string_view param);

static std::string_view CmdLogoff(Context &ctx, std::string_view param);

static std::string_view CmdSignal(Context &ctx, std::string_view param);

using command_func = std::function<std::string_view(Context &, std::string_view)>;

extern std::map<std::string, command_func> cmd_funcs_;

std::string_view Command(Context &context, std::string cmd, std::string_view param);
