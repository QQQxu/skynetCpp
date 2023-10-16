#pragma once

#include <string>

struct Config {
  int thread;
  int harbor;
  int profile;
  std::string daemon;
  std::string module_path;
  std::string bootstrap_name;
  std::string bootstrap_args;
  std::string logger;
  std::string logservice;
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4