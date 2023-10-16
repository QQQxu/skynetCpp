#include "env.h"

#include <cassert>
#include <cstdlib>
#include <string_view>

#include "skynet.h"
// #include "spinlock.h"



struct Env {
  // struct Spinlock lock;
};

static struct Env *E = NULL;

std::string_view GetEnv(std::string_view key) {
  return "";
}

void SetEnv(std::string_view key, std::string_view value) {
}

void EnvInit() {
}
