#include "module.h"

#include <dlfcn.h>
#include <cassert>
#include <stdexcept>
#include <string>

#include "skynet.h"
#include "spinlock.h"

void *ModuleCreate(Module &mod) {
  if (mod.create == nullptr) {
    throw std::runtime_error("Module does not have a create function");
  }
  return mod.create();
}

int ModuleInit(Module &mod, void *inst, Context *ctx, std::string_view parm) {
  if (mod.init == nullptr) {
    throw std::runtime_error("Module does not have an init function");
  }
  return mod.init(inst, ctx, parm);
}

void ModuleRelease(Module &mod, void *inst) {
  if (mod.release == nullptr) {
    throw std::runtime_error("Module does not have an release function");
  }
  return mod.release(inst);
}

void ModuleSignal(Module &mod, void *inst, int signal) {
  if (mod.signal == nullptr) {
    throw std::runtime_error("Module does not have an signal function");
  }
  return mod.signal(inst, signal);
}

/******************** Modules ********************************/

static Modules kGlobalModules;

void SkynetModuleInit(std::string_view path) {
  kGlobalModules.count_ = 0;
  kGlobalModules.path_ = path;
  spinlock_init(&kGlobalModules.lock);
}

/*
查询并加载指定模块，若不存在对应模块，则创建并加入全局模块管理中
*/
Module *Modules::Query(const std::string &mod_name) {
  auto it = kGlobalModules.modules_.find(mod_name);
  if (it != kGlobalModules.modules_.end()) {
    return &it->second;
  }

  spinlock_init(&kGlobalModules.lock);

  it = kGlobalModules.modules_.find(mod_name);  // double check

  if (it == kGlobalModules.modules_.end() && kGlobalModules.count_ < kMaxModuleType) {
    Module mod = kGlobalModules.TryOpenSO(kGlobalModules.path_, mod_name);
    kGlobalModules.modules_[std::string(mod_name)] = mod;
    kGlobalModules.count_++;
    it = kGlobalModules.modules_.find(mod_name);
  }

  spinlock_unlock(&kGlobalModules.lock);

  return &it->second;
  ;
}

static void *GetAPI(struct Module *mod, const char *api_name) {
  std::string ptr = mod->name_.substr(3) + std::string(api_name);
  return dlsym(mod->module_, ptr.c_str());
}

/*
尝试加载动态库(服务模块) 
:param:path: 动态库文件路径，通常默认为 ./service
*/
Module Modules::TryOpenSO(const std::string &path, const std::string &name) {
  std::string filename = path + '/' + name + ".so";

  void *dl = dlopen(filename.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (dl == nullptr) {
    throw std::runtime_error("Can't open " + name + ":" + dlerror());
  }

  Module mod;
  mod.name_ = name;
  mod.module_ = dl;

  *(void **)&mod.create = GetAPI(&mod, "_create");
  *(void **)&mod.init = GetAPI(&mod, "_init");
  *(void **)&mod.release = GetAPI(&mod, "_release");
  *(void **)&mod.signal = GetAPI(&mod, "_signal");

  return mod;
}
