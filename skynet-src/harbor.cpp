#include "harbor.h"

#include <cassert>

#include "handle.h"
#include "skynet.h"

static struct Context *kRemote = nullptr;
static unsigned int kHarbor = ~0;

/*
消息发送
remoet_message: 消息结构体
source:
session:
*/
void HarborSend(struct RemoteMessage *remoet_message, uint32_t source, int session) {
  assert(remoet_message->type != kPtypeSystem && remoet_message->type != kPtypeHarbor && kHarbor);
  kRemote->Send(remoet_message, sizeof(*remoet_message), source, kPtypeSystem, session);
}

int HarborMessageIsRemote(uint32_t handle) {
  assert(kHarbor != ~0);
  int h = (handle & ~kHandleMask);
  return h != kHarbor && h != 0;
}

void HarborInit(int harbor) { 
  kHarbor = (unsigned int)harbor << kHandleRemoteShift;
}

void HarborStart(Context *ctx) {
  // the HARBOR must be reserved to ensure the pointer is valid.
  // It will be released at last by calling skynet_harbor_exit
  ctx->Reserve();
  kRemote = ctx;
}

void HarborExit() {
  Context *ctx = kRemote;
  kRemote = nullptr;
  if (ctx) {
    ctx->Release();
  }
}
