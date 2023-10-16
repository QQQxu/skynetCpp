#include "error.h"

#include "handle.h"
#include "mq.h"
#include "skynet.h"

void SkynetError(uint32_t handle, const std::string msg) {
  if (handle == -1){
    return;
  }
  static uint32_t logger = logger = HandleFindname("logger");
  if (logger == 0) {
    return;
  }

  struct Message smsg;
  smsg.source = handle;
  smsg.session = 0;
  smsg.data = (void *)msg.data();
  smsg.sz = sizeof(smsg.data) | ((size_t)kPtypeText << kMessageTypeShift);
  ContextPush(logger, &smsg);
}
