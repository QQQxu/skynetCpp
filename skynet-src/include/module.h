#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "spinlock.h"

constexpr int kMaxModuleType = 32;

class Context;

struct Module {
  std::string name_;
  void *module_;

  void *(*create)(void);
  int (*init)(void *, Context *, std::string_view);
  void (*release)(void *);
  void (*signal)(void *, int);
};

void *ModuleCreate(Module &mod);
int ModuleInit(Module &mod, void *inst, Context *ctx, std::string_view parm);
void ModuleRelease(Module &mod, void *inst);
void ModuleSignal(Module &mod, void *inst, int signal);

class Modules {
 public:
  int count_;
  struct Spinlock lock;
  std::string path_;  // 通常默认为 "./service"

  static Module *Query(const std::string &mod_name);

 private:
  std::unordered_map<std::string, Module> modules_;

  Module TryOpenSO(const std::string &path, const std::string &name);
};

void SkynetModuleInit(std::string_view path);