#pragma once

#include <cstdint>

#define SOCKET_INFO_UNKNOWN 0
#define SOCKET_INFO_LISTEN 1
#define SOCKET_INFO_TCP 2
#define SOCKET_INFO_UDP 3
#define SOCKET_INFO_BIND 4
#define SOCKET_INFO_CLOSING 5

class SocketInfo {
 public:
  SocketInfo* CreateNext(SocketInfo *last);
  void Release(SocketInfo *socket_info);

 public:
  int id_;
  int type_;
  uint64_t opaque_;  // 不透明
  uint64_t read_;
  uint64_t write_;
  uint64_t rtime_;
  uint64_t wtime_;
  int64_t wbuffer_;
  uint8_t reading_;
  uint8_t writing_;
  char name[128];
  SocketInfo *next;
};
