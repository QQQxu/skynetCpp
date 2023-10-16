#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "start.h"
#include "env.h"
#include "imp.h"
#include "service.h"
#include "skynet.h"
#include "ssignal.h"
#include "config.h"

int main(int argc, char *argv[]) {
  GlobalInit();
  EnvInit();

  Sigign();

  struct Config config;
	config.thread = 8;
	config.harbor = 1;
	config.profile = 1;
	config.module_path =  std::string(PROJECT_PATH) + std::string("/service-src");
	config.daemon =  "";
	config.logger = "";
	config.logservice = "liblogger";

  SkynetStart(&config);
  SkynetExit();
  return 0;
}
