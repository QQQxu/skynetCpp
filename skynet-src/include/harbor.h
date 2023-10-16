#pragma once

#include <cstdint>
#include <cstdlib>

#include "service.h"

constexpr int kGlobalNameLength = 16;
constexpr int kRemoteMax = 256;

/*
`harbor`模块的主要作用是初始化节点模块，用于集群，转发远程节点的消息。
这意味着它负责处理集群中不同节点之间的消息传递，使得整个系统能够作为一个整体运行。
每个skynet节点可配置一个唯一harbor，用于与其他节点组网
*/

struct RemoteName {
  char name[kGlobalNameLength];
  uint32_t handle;
};

struct RemoteMessage {
  struct RemoteName destination;
  const void *message;
  size_t sz;
  int type;
};

void HarborSend(struct RemoteMessage *rmsg, uint32_t source, int session);
int HarborMessageIsRemote(uint32_t handle);
void HarborInit(int harbor);
void HarborStart(Context *ctx);
void HarborExit();