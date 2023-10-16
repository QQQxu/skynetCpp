#include "socket_epoll.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

bool EpollInvalid(int efd) { return efd == -1; }

int EpollCreate() { return epoll_create1(0); }

void EpollClose(int efd) { close(efd); }

/*
param: sock: 监听对象
*/
int EpollAdd(poll_fd efd, int sock, struct Socket *data) {
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.ptr = data;
  if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
    return 1;
  }
  return 0;
}

void EpollDel(int efd, int sock) { epoll_ctl(efd, EPOLL_CTL_DEL, sock, nullptr); }

int EpollEnable(int efd, int sock, struct Socket *data, bool read_enable, bool write_enable) {
  struct epoll_event ev;
  ev.events = (read_enable ? EPOLLIN : 0) | (write_enable ? EPOLLOUT : 0);
  ev.data.ptr = data;
  if (epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev) == -1) {
    return 1;
  }
  return 0;
}



void EpollNonblocking(int fd) {
  int flag = fcntl(fd, F_GETFL, 0);
  if (-1 == flag) {
    return;
  }

  fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}
