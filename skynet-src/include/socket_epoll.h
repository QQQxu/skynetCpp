#pragma once

#include <cstddef>
#include <sys/epoll.h>

typedef int poll_fd;

struct Socket;

struct Event {
  struct Socket *s;
  bool read;
  bool write;
  bool error;
  bool eof;
};

bool EpollInvalid(poll_fd fd);
poll_fd EpollCreate();
void EpollClose(poll_fd fd);
int EpollAdd(poll_fd fd, int sock, struct Socket *data);
void EpollDel(poll_fd fd, int sock);
int EpollEnable(poll_fd, int sock, struct Socket *data, bool read_enable, bool write_enable);

template <typename T, std::size_t N>
int EpollWait(poll_fd efd, T (&event)[N]) {
  struct epoll_event ev[N];
  int n = epoll_wait(efd, ev, N, -1);
  for (int i = 0; i < n; i++) {
    event[i].s = (struct Socket *)ev[i].data.ptr;
    unsigned flag = ev[i].events;
    event[i].write = (flag & EPOLLOUT) != 0;
    event[i].read = (flag & EPOLLIN) != 0;
    event[i].error = (flag & EPOLLERR) != 0;
    event[i].eof = (flag & EPOLLHUP) != 0;
  }
  return n;
}

void EpollNonblocking(int sock);
