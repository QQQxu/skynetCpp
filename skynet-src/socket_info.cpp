#include "socket_info.h"

SocketInfo *SocketInfo::CreateNext(SocketInfo *last) {
  SocketInfo *socket_info = new SocketInfo();
  socket_info->next = last;
  return socket_info;
}

void Release(SocketInfo *socket_info) {
  while (socket_info) {
    SocketInfo *temp = socket_info;
    socket_info = socket_info->next;
    delete temp;
  }
}