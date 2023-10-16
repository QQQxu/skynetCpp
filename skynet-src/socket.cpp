#include "socket.h"

#include "harbor.h"
#include "mq.h"
#include "service.h"
#include "skynet.h"
#include "socket_server.h"
#include "timer.h"
#include "error.h"

#include <cassert>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

static SocketServer *kSocketServer;

void SocketInit() { kSocketServer = CreateSocketServer(Now()); }

void SocketExit() { delete kSocketServer; }

void SocketFree() { kSocketServer = nullptr; }

void SocketUpdatetime() { kSocketServer->UpdateTime(Now()); }

// mainloop thread
static void ForwardMessage(int type, bool padding, struct SocketMessage &socket_message) {
  struct SkynetSocketMessage *sm;
  size_t sz = sizeof(*sm);
  if (padding) {
    if (!socket_message.data.empty()) {
      std::string::size_type msg_size = socket_message.data.size();
      if (msg_size > 128) {
        msg_size = 128;
      }
      sz += msg_size;
    } else {
      socket_message.data.clear();
    }
  }
  sm = new SkynetSocketMessage();
  sm->type = type;
  sm->id = socket_message.id;
  sm->ud = socket_message.ud;
  if (padding) {
    sm->buffer = nullptr;
    // memcpy(sm + 1, result->data, sz - sizeof(*sm));
  } else {
    sm->buffer = socket_message.data;
  }

  struct Message message;
  message.source = 0;
  message.session = 0;
  message.data = sm;
  message.sz = sz | ((size_t)kPtypeSocket << kMessageTypeShift);

  if (ContextPush((uint32_t)socket_message.opaque, &message)) {
    // todo: report somewhere to close socket
    // don't call SocketClose here (It will block mainloop)
    delete sm;
  }
}

int SocketPoll() {
  assert(kSocketServer);
  struct SocketMessage socket_message;
  int more = 1;
  int type = kSocketServer->Poll(socket_message, &more);
  switch (type) {
    case kSocketExit:
      return 0;
    case kSocketData:
      ForwardMessage(kSocketTypeData, false, socket_message);
      break;
    case kSocketClose:
      ForwardMessage(kSocketTypeClose, false, socket_message);
      break;
    case kSocketOpen:
      ForwardMessage(kSocketTypeConnect, true, socket_message);
      break;
    case kSocketErr:
      ForwardMessage(kSocketTypeError, true, socket_message);
      break;
    case kSocketAccept:
      ForwardMessage(kSocketTypeAccept, true, socket_message);
      break;
    case kSocketUDP:
      ForwardMessage(kSocketTypeUdp, false, socket_message);
      break;
    case kSocketWarning:
      ForwardMessage(kSocketTypeWarning, false, socket_message);
      break;
    default:
      SkynetError(-1, "Unknown socket message type " +  std::to_string(type));
      return -1;
  }
  if (more) {
    return -1;
  }
  return 1;
}

int SocketSendBuffer(struct Sendbuffer &socket_sendbuffer) { return kSocketServer->Send(socket_sendbuffer); }

int SocketSendBufferLowpriority(struct Sendbuffer &socket_sendbuffer) {
  return kSocketServer->SendLowpriority(socket_sendbuffer);
}

int SocketListen(Context *ctx, const char *host, int port, int backlog) {
  uint32_t source = ctx->GetHandle();
  return kSocketServer->Listen(source, host, port, backlog);
}

int SocketConnect(Context *ctx, const char *host, int port) {
  uint32_t source = ctx->GetHandle();
  return kSocketServer->Connect(source, host, port);
}

int SocketBind(Context *ctx, int fd) {
  uint32_t source = ctx->GetHandle();
  return kSocketServer->Bind(source, fd);
}

void SocketClose(Context *ctx, int id) {
  uint32_t source = ctx->GetHandle();
  kSocketServer->Close(source, id);
}

void SocketShutdown(Context *ctx, int id) {
  uint32_t source = ctx->GetHandle();
  kSocketServer->Shutdown(source, id);
}

void SocketStart(Context *ctx, int id) {
  uint32_t source = ctx->GetHandle();
  kSocketServer->Start(source, id);
}

void SocketPause(Context *ctx, int id) {
  uint32_t source = ctx->GetHandle();
  kSocketServer->Pause(source, id);
}

void SocketNodelay(Context *ctx, int id) { kSocketServer->Nodelay(id); }

int SocketUDP(Context *ctx, const char *addr, int port) {
  uint32_t source = ctx->GetHandle();
  return kSocketServer->UDP(source, addr, port);
}

int SocketUdpConnect(Context *ctx, int id, const char *addr, int port) {
  return kSocketServer->UDPConnect(id, addr, port);
}

int SocketUdpSendBuffer(Context *ctx, std::string address, struct Sendbuffer &buffer) {
  return kSocketServer->UDPSend(address, buffer);
}

std::vector<int> SocketUdpAddress(struct SkynetSocketMessage &msg, int *addrsz) {
  if (msg.type != kSocketTypeUdp) {
    return {};
  }
  struct SocketMessage sm;
  sm.id = msg.id;
  sm.opaque = 0;
  sm.ud = msg.ud;
  sm.data = msg.buffer;
  return kSocketServer->UdpAddress(sm, addrsz);
}

SocketInfo *GetSocketInfo() { return kSocketServer->Info(); }
