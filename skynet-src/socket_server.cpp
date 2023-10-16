#include "socket_server.h"

#include <arpa/inet.h>
#include <error.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdbool>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "skynet.h"

static inline void SocketLockInit(struct Socket &sock, struct SocketLock &socket_lock) {
  socket_lock.lock = &sock.dw_lock;
  socket_lock.count = 0;
}

static inline void SocketLock(struct SocketLock &socket_lock) {
  if (socket_lock.count == 0) {
    spinlock_lock(socket_lock.lock);
  }
  ++socket_lock.count;
}

static inline int SocketTrylock(struct SocketLock &socket_lock) {
  if (socket_lock.count == 0) {
    if (!spinlock_trylock(socket_lock.lock)) return 0;  // lock failed
  }
  ++socket_lock.count;
  return 1;
}

static inline void SocketUnlock(struct SocketLock &socket_lock) {
  --socket_lock.count;
  if (socket_lock.count <= 0) {
    assert(socket_lock.count == 0);
    spinlock_unlock(socket_lock.lock);
  }
}

static inline int SocketInvalid(struct Socket &sock, int id) {
  return (sock.id != id || std::atomic_load(&sock.type) == kSocketTypeInvalid);
}

static inline void SendObjectInitFromSendbuffer(struct SendObject &send_object,
                                                struct Sendbuffer &socket_sendbuffer) {
  switch (socket_sendbuffer.type) {
    case SOCKET_BUFFER_MEMORY:
      send_object.buffer = socket_sendbuffer.buffer;
      send_object.sz = socket_sendbuffer.sz;
      break;
    case SOCKET_BUFFER_OBJECT:
      send_object.buffer = socket_sendbuffer.buffer;
      send_object.sz = socket_sendbuffer.sz;
      break;
    case SOCKET_BUFFER_RAWPOINTER:
      send_object.buffer = socket_sendbuffer.buffer;
      send_object.sz = socket_sendbuffer.sz;
      break;
    default:
      // never get here
      send_object.buffer = nullptr;
      send_object.sz = 0;
      break;
  }
}

static void SocketKeepalive(int fd) {
  int keepalive = 1;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
}

static int ReserveId(SocketServer &sock_srv) {
  int i;
  for (i = 0; i < MAX_SOCKET; i++) {
    int id = std::atomic_fetch_add(&(sock_srv.alloc_id_), decltype((&(sock_srv.alloc_id_))->load())(1)) + 1;
    if (id < 0) {
      id = std::atomic_fetch_and(&(sock_srv.alloc_id_), decltype((&(sock_srv.alloc_id_))->load())(0x7fffffff)) &
           0x7fffffff;
    }
    struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
    int type_invalid = std::atomic_load(&sock->type);
    if (type_invalid == kSocketTypeInvalid) {
      if (std::atomic_compare_exchange_weak(&sock->type, &type_invalid, kSocketTypeReserve)) {
        sock->id = id;
        sock->protocol = Protocol::UNKNOWN;
        // socket_server_udp_connect may inc sock->udpconncting directly (from other thread, before NewFd),
        // so reset it to 0 here rather than in NewFd.
        std::atomic_init(&sock->udpconnecting, 0);
        sock->fd = -1;
        return id;
      } else {
        // retry
        --i;
      }
    }
  }
  return -1;
}

static inline void ClearWriteBufferList(struct WriteBufferList &list) {
  list.head = nullptr;
  list.tail = nullptr;
}

static void FreeWriteBufferList(SocketServer &socket_server, struct WriteBufferList &write_buffer_listlist) {
  struct WriteBuffer *writw_buffer = write_buffer_listlist.head;
  while (writw_buffer) {
    struct WriteBuffer *tmp = writw_buffer;
    writw_buffer = writw_buffer->next;
  }
  write_buffer_listlist.head = nullptr;
  write_buffer_listlist.tail = nullptr;
}

static void FreeBuffer(SocketServer &socket_server, struct Sendbuffer &socket_sendbuffer) {
  switch (socket_sendbuffer.type) {
    case SOCKET_BUFFER_MEMORY:
      delete socket_sendbuffer.buffer;
      break;
    case SOCKET_BUFFER_OBJECT:
      break;
    case SOCKET_BUFFER_RAWPOINTER:
      break;
  }
}

static const std::string *CloneBuffer(struct Sendbuffer &socket_sendbuffer, size_t *size) {
  switch (socket_sendbuffer.type) {
    case SOCKET_BUFFER_MEMORY:
      *size = socket_sendbuffer.sz;
      return socket_sendbuffer.buffer;
    case SOCKET_BUFFER_OBJECT:
      *size = USEROBJECT;
      return socket_sendbuffer.buffer;
      // case SOCKET_BUFFER_RAWPOINTER:
      //   // It's a raw pointer, we need make a copy
      //   *size = socket_sendbuffer.sz;
      //   void *tmp;
      //   void *tmp;  // = MALLOC(*size);
      //   memcpy(tmp, socket_sendbuffer.buffer, *size);
      //   return tmp;
  }
  // never get here
  *size = 0;
  return nullptr;
}

static void ForceClose(SocketServer &socket_server, struct Socket &socket, struct SocketLock &socket_lock,
                       struct SocketMessage &socket_message) {
  socket_message.id = socket.id;
  socket_message.ud = 0;
  socket_message.data = nullptr;
  socket_message.opaque = socket.opaque;
  uint8_t type = std::atomic_load(&socket.type);
  if (type == kSocketTypeInvalid) {
    return;
  }
  assert(type != kSocketTypeReserve);
  FreeWriteBufferList(socket_server, socket.high);
  FreeWriteBufferList(socket_server, socket.low);
  EpollDel(socket_server.event_fd_, socket.fd);
  SocketLock(socket_lock);
  if (type != kSocketTypeBind) {
    if (close(socket.fd) < 0) {
      perror("close socket:");
    }
  }
  std::atomic_store(&socket.type, kSocketTypeInvalid);
  if (socket.dw_buffer) {
    struct Sendbuffer tmp;
    tmp.buffer = socket.dw_buffer;
    tmp.sz = socket.dw_size;
    tmp.id = socket.id;
    tmp.type = (tmp.sz == USEROBJECT) ? SOCKET_BUFFER_OBJECT : SOCKET_BUFFER_MEMORY;
    FreeBuffer(socket_server, tmp);
    socket.dw_buffer = NULL;
  }
  SocketUnlock(socket_lock);
}

static inline void CheckWriteBufferList(struct WriteBufferList *write_buffer_list) {
  assert(write_buffer_list->head == nullptr);
  assert(write_buffer_list->tail == nullptr);
}

static inline int EnableWrite(SocketServer &socket_server, struct Socket &socket, bool enable) {
  if (socket.writing != enable) {
    socket.writing = enable;
    return EpollEnable(socket_server.event_fd_, socket.fd, &socket, socket.reading, enable);
  }
  return 0;
}

static inline int EnableRead(SocketServer &socket_server, struct Socket &socket, bool enable) {
  if (socket.reading != enable) {
    socket.reading = enable;
    return EpollEnable(socket_server.event_fd_, socket.fd, &socket, enable, socket.writing);
  }
  return 0;
}

static struct Socket *NewFd(SocketServer &socket_server, int id, int fd, Protocol protocol, uintptr_t opaque,
                            bool reading) {
  struct Socket *sock = &socket_server.slot_[HASH_ID(id)];
  assert(std::atomic_load(&sock->type) == kSocketTypeReserve);

  if (EpollAdd(socket_server.event_fd_, fd, sock)) {
    std::atomic_store(&sock->type, kSocketTypeInvalid);
    return NULL;
  }

  sock->id = id;
  sock->fd = fd;
  sock->reading = true;
  sock->writing = false;
  sock->closing = false;
  std::atomic_init(&sock->sending, ID_TAG16(id) << 16 | 0);
  sock->protocol = protocol;
  sock->p = MIN_READ_BUFFER;
  sock->opaque = opaque;
  sock->wb_size = 0;
  sock->warn_size = 0;
  CheckWriteBufferList(&sock->high);
  CheckWriteBufferList(&sock->low);
  sock->dw_buffer = NULL;
  sock->dw_size = 0;
  memset(&sock->stat, 0, sizeof(sock->stat));
  if (EnableRead(socket_server, *sock, reading)) {
    std::atomic_store(&sock->type, kSocketTypeInvalid);
    return NULL;
  }
  return sock;
}

static inline void StatRead(SocketServer &sock_srv, struct Socket &sock, int n) {
  sock.stat.read += n;
  sock.stat.rtime = sock_srv.time_;
}

static inline void StatWrite(SocketServer &sock_srv, struct Socket &sock, int n) {
  sock.stat.write += n;
  sock.stat.wtime = sock_srv.time_;
}

// return -1 when connecting
static int OpenSocket(SocketServer &sock_srv, struct RequestOpen &request, struct SocketMessage &socket_message) {
  int id = request.id;
  socket_message.opaque = request.opaque;
  socket_message.id = id;
  socket_message.ud = 0;
  socket_message.data = nullptr;
  int status;
  struct addrinfo ai_hints;
  struct addrinfo *ai_list = nullptr;
  struct addrinfo *ai_ptr = nullptr;
  char port[16];
  sprintf(port, "%d", request.port);
  memset(&ai_hints, 0, sizeof(ai_hints));
  ai_hints.ai_family = AF_UNSPEC;
  ai_hints.ai_socktype = SOCK_STREAM;
  ai_hints.ai_protocol = IPPROTO_TCP;

  status = getaddrinfo(request.host, port, &ai_hints, &ai_list);
  if (status != 0) {
    socket_message.data = (char *)gai_strerror(status);
    std::atomic_store(&sock_srv.slot_[HASH_ID(id)].type, kSocketTypeInvalid);
    return kSocketErr;
  }
  int sock = -1;
  for (ai_ptr = ai_list; ai_ptr != nullptr; ai_ptr = ai_ptr->ai_next) {
    sock = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
    if (sock < 0) {
      continue;
    }
    SocketKeepalive(sock);
    EpollNonblocking(sock);
    status = connect(sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
    if (status != 0 && errno != EINPROGRESS) {
      close(sock);
      sock = -1;
      continue;
    }
    break;
  }

  if (sock < 0) {
    socket_message.data = strerror(errno);
    freeaddrinfo(ai_list);
    std::atomic_store(&sock_srv.slot_[HASH_ID(id)].type, kSocketTypeInvalid);
    return kSocketErr;
  }

  struct Socket *new_socket = NewFd(sock_srv, id, sock, Protocol::TCP, request.opaque, true);
  if (new_socket == nullptr) {
    socket_message.data = "reach skynet socket number limit";
    freeaddrinfo(ai_list);
    std::atomic_store(&sock_srv.slot_[HASH_ID(id)].type, kSocketTypeInvalid);
    return kSocketErr;
  }

  if (status == 0) {
    std::atomic_store(&new_socket->type, kSocketTypeConnected);
    struct sockaddr *addr = ai_ptr->ai_addr;
    void *sin_addr = (ai_ptr->ai_family == AF_INET) ? (void *)&((struct sockaddr_in *)addr)->sin_addr
                                                    : (void *)&((struct sockaddr_in6 *)addr)->sin6_addr;
    if (inet_ntop(ai_ptr->ai_family, sin_addr, sock_srv.buffer_, sizeof(sock_srv.buffer_))) {
      socket_message.data = sock_srv.buffer_;
    }
    freeaddrinfo(ai_list);
    return kSocketOpen;
  } else {
    if (EnableWrite(sock_srv, *new_socket, true)) {
      socket_message.data = "enable write failed";
      close(sock);
      freeaddrinfo(ai_list);
      std::atomic_store(&sock_srv.slot_[HASH_ID(id)].type, kSocketTypeInvalid);
      return kSocketErr;
    }
    std::atomic_store(&new_socket->type, kSocketTypeConnecting);
  }

  freeaddrinfo(ai_list);
  return -1;
}

static int ReportError(struct Socket &sock, struct SocketMessage &msg, const char *err) {
  msg.id = sock.id;
  msg.ud = 0;
  msg.opaque = sock.opaque;
  msg.data = (char *)err;
  return kSocketErr;
}

static int CloseWrite(SocketServer &sock_srv, struct Socket &sock, struct SocketLock &socket_lock,
                      struct SocketMessage &socket_message) {
  if (sock.closing) {
    ForceClose(sock_srv, sock, socket_lock, socket_message);
    return kSocketRst;
  } else {
    int t = std::atomic_load(&sock.type);
    if (t == kSocketTypeHalfCloseRead) {
      // recv 0 before, ignore the error and close fd
      ForceClose(sock_srv, sock, socket_lock, socket_message);
      return kSocketRst;
    }
    if (t == kSocketTypeHalfCloseWrite) {
      // already raise SOCKET_ERR
      return kSocketRst;
    }
    std::atomic_store(&sock.type, kSocketTypeHalfCloseWrite);
    shutdown(sock.fd, SHUT_WR);
    EnableWrite(sock_srv, sock, false);
    return ReportError(sock, socket_message, strerror(errno));
  }
}

static int SendListTCP(SocketServer &sock_srv, struct Socket &sock, struct WriteBufferList &write_buffer_list,
                       struct SocketLock &socket_lock, struct SocketMessage &socket_message) {
  while (write_buffer_list.head) {
    struct WriteBuffer *tmp = write_buffer_list.head;
    while (true) {
      ssize_t sz = write(sock.fd, tmp->ptr, tmp->sz);
      if (sz < 0) {
        switch (errno) {
          case EINTR:
            continue;
          case AGAIN_WOULDBLOCK:
            return -1;
        }
        return CloseWrite(sock_srv, sock, socket_lock, socket_message);
      }
      StatWrite(sock_srv, sock, (int)sz);
      sock.wb_size -= sz;
      if (sz != tmp->sz) {
        tmp->ptr += sz;
        tmp->sz -= sz;
        return -1;
      }
      break;
    }
    write_buffer_list.head = tmp->next;
  }
  write_buffer_list.tail = NULL;

  return -1;
}

static socklen_t UDPSocketAddress(struct Socket &sock, const uint8_t udp_address[kUdpAddressSize],
                                  union kSockaddrAll &sa) {
  Protocol protocol = (Protocol)udp_address[0];
  if (protocol != sock.protocol) return 0;
  uint16_t port = 0;
  memcpy(&port, udp_address + 1, sizeof(uint16_t));
  switch (sock.protocol) {
    case Protocol::UDP:
      memset(&sa.v4, 0, sizeof(sa.v4));
      sa.s.sa_family = AF_INET;
      sa.v4.sin_port = port;
      memcpy(&sa.v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa.v4.sin_addr));  // ipv4 address is 32 bits
      return sizeof(sa.v4);
    case Protocol::UDPv6:
      memset(&sa.v6, 0, sizeof(sa.v6));
      sa.s.sa_family = AF_INET6;
      sa.v6.sin6_port = port;
      memcpy(&sa.v6.sin6_addr, udp_address + 1 + sizeof(uint16_t),
             sizeof(sa.v6.sin6_addr));  // ipv6 address is 128 bits
      return sizeof(sa.v6);
    default:
      return 0;
  }
}

static void DropUDP(SocketServer &sock_srv, struct Socket &sock, struct WriteBufferList &write_buffer_list,
                    struct WriteBuffer &write_buffer) {
  sock.wb_size -= write_buffer.sz;
  write_buffer_list.head = write_buffer.next;
  if (write_buffer_list.head == nullptr) write_buffer_list.tail = nullptr;
  delete &write_buffer;
}

static int SendListUDP(SocketServer &sock_srv, struct Socket &sock, struct WriteBufferList &write_buffer_list,
                       struct SocketMessage &socket_message) {
  while (write_buffer_list.head) {
    struct WriteBuffer *write_buffer = write_buffer_list.head;
    struct WriteBufferUDP *udp = (struct WriteBufferUDP *)write_buffer;
    union kSockaddrAll sa;
    socklen_t sasz = UDPSocketAddress(sock, udp->udp_address, sa);
    if (sasz == 0) {
      // skynet_error(NULL, "socket-server : udp (%d) type mismatch.", sock.id);
      DropUDP(sock_srv, sock, write_buffer_list, *write_buffer);
      return -1;
    }
    int err = sendto(sock.fd, write_buffer->ptr, write_buffer->sz, 0, &sa.s, sasz);
    if (err < 0) {
      switch (errno) {
        case EINTR:
        case AGAIN_WOULDBLOCK:
          return -1;
      }
      // skynet_error(NULL, "socket-server : udp (%d) sendto error %s.", sock.id, strerror(errno));
      DropUDP(sock_srv, sock, write_buffer_list, *write_buffer);
      return -1;
    }
    StatWrite(sock_srv, sock, write_buffer->sz);
    sock.wb_size -= write_buffer->sz;
    write_buffer_list.head = write_buffer->next;
  }
  write_buffer_list.tail = nullptr;

  return -1;
}

static int SendList(SocketServer &sock_srv, struct Socket &sock, struct WriteBufferList &write_buffer_list,
                    struct SocketLock &socket_lock, struct SocketMessage &socket_message) {
  if (sock.protocol == Protocol::TCP) {
    return SendListTCP(sock_srv, sock, write_buffer_list, socket_lock, socket_message);
  } else {
    return SendListUDP(sock_srv, sock, write_buffer_list, socket_message);
  }
}

static inline int ListUncomplete(struct WriteBufferList &write_buffer_list) {
  struct WriteBuffer *write_buffer = write_buffer_list.head;
  if (write_buffer == nullptr) return 0;

  return write_buffer->ptr != write_buffer->buffer->c_str();
}

static void RaiseUncomplete(struct Socket &sock) {
  struct WriteBufferList *low = &sock.low;
  struct WriteBuffer *tmp = low->head;
  low->head = tmp->next;
  if (low->head == NULL) {
    low->tail = NULL;
  }

  // move head of low list (tmp) to the empty high list
  struct WriteBufferList *high = &sock.high;
  assert(high->head == NULL);

  tmp->next = NULL;
  high->head = high->tail = tmp;
}

static inline int SendBufferEmpty(struct Socket &sock) {
  return (sock.high.head == nullptr && sock.low.head == nullptr);
}

/*
        Each socket has two write buffer list, high priority and low priority.

        1. send high list as far as possible.
        2. If high list is empty, try to send low list.
        3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call
   RaiseUncomplete) .
        4. If two lists are both empty, turn off the event. (call check_close)
 */
static int SendBuffer_(SocketServer &sock_srv, struct Socket &sock, struct SocketLock &socket_lock,
                       struct SocketMessage &socket_message) {
  assert(!ListUncomplete(sock.low));
  // step 1
  int ret = SendList(sock_srv, sock, sock.high, socket_lock, socket_message);
  if (ret != -1) {
    if (ret == kSocketErr) {
      // HALFCLOSE_WRITE
      return kSocketErr;
    }
    // SOCKET_RST (ignore)
    return -1;
  }
  if (sock.high.head == NULL) {
    // step 2
    if (sock.low.head != NULL) {
      int ret = SendList(sock_srv, sock, sock.low, socket_lock, socket_message);
      if (ret != -1) {
        if (ret == kSocketErr) {
          // HALFCLOSE_WRITE
          return kSocketErr;
        }
        // SOCKET_RST (ignore)
        return -1;
      }
      // step 3
      if (ListUncomplete(sock.low)) {
        RaiseUncomplete(sock);
        return -1;
      }
      if (sock.low.head) return -1;
    }
    // step 4
    assert(SendBufferEmpty(sock) && sock.wb_size == 0);

    if (sock.closing) {
      // finish writing
      ForceClose(sock_srv, sock, socket_lock, socket_message);
      return -1;
    }

    int err = EnableWrite(sock_srv, sock, false);

    if (err) {
      return ReportError(sock, socket_message, "disable write failed");
    }

    if (sock.warn_size > 0) {
      sock.warn_size = 0;
      socket_message.opaque = sock.opaque;
      socket_message.id = sock.id;
      socket_message.ud = 0;
      socket_message.data = nullptr;
      return kSocketWarning;
    }
  }

  return -1;
}

static int SendBuffer(SocketServer &sock_srv, struct Socket &sock, struct SocketLock &socket_lock,
                      struct SocketMessage &socket_message) {
  if (!SocketTrylock(socket_lock)) return -1;  // blocked by direct write, send later.
  if (sock.dw_buffer) {
    // add direct write buffer before high.head
    struct WriteBuffer *write_buffer = new WriteBuffer();
    struct SendObject send_object;
    send_object.buffer = sock.dw_buffer;
    send_object.sz = sock.dw_size;

    write_buffer->ptr = (char *)send_object.buffer + sock.dw_offset;
    write_buffer->sz = send_object.sz - sock.dw_offset;
    write_buffer->buffer = sock.dw_buffer;
    sock.wb_size += write_buffer->sz;
    if (sock.high.head == nullptr) {
      sock.high.head = sock.high.tail = write_buffer;
      write_buffer->next = nullptr;
    } else {
      write_buffer->next = sock.high.head;
      sock.high.head = write_buffer;
    }
    sock.dw_buffer = nullptr;
  }
  int r = SendBuffer_(sock_srv, sock, socket_lock, socket_message);
  SocketUnlock(socket_lock);

  return r;
}

static struct WriteBuffer *AppendSendbuffer_(SocketServer &sock_srv, struct WriteBufferList &write_buffer_list,
                                             struct RequestSend &request_send, int size) {
  struct WriteBuffer *buf;  // = MALLOC(size);
  struct SendObject send_object;
  send_object.buffer = request_send.buffer;
  send_object.sz = request_send.sz;
  buf->ptr = (char *)send_object.buffer;
  buf->sz = send_object.sz;
  buf->buffer = request_send.buffer;
  buf->next = NULL;
  if (write_buffer_list.head == NULL) {
    write_buffer_list.head = write_buffer_list.tail = buf;
  } else {
    assert(write_buffer_list.tail != NULL);
    assert(write_buffer_list.tail->next == NULL);
    write_buffer_list.tail->next = buf;
    write_buffer_list.tail = buf;
  }
  return buf;
}

static inline void AppendSendbufferUdp(SocketServer &sock_srv, struct Socket &sock, int priority,
                                       struct RequestSend &request_send, const uint8_t udp_address[kUdpAddressSize]) {
  struct WriteBufferList *write_buffer_list = (priority == PRIORITY_HIGH) ? &sock.high : &sock.low;
  struct WriteBufferUDP *write_buffer_UDP = new WriteBufferUDP();
  write_buffer_UDP->buffer = *AppendSendbuffer_(sock_srv, *write_buffer_list, request_send, sizeof(*write_buffer_UDP));
  memcpy(write_buffer_UDP->udp_address, udp_address, kUdpAddressSize);
  sock.wb_size += write_buffer_UDP->buffer.sz;
}

static inline void AppendSendbuffer(SocketServer &sock_srv, struct Socket &sock, struct RequestSend &request_send) {
  struct WriteBuffer *buf = AppendSendbuffer_(sock_srv, sock.high, request_send, sizeof(*buf));
  sock.wb_size += buf->sz;
}

static inline void AppendSendbufferLow(SocketServer &sock_srv, struct Socket &sock, struct RequestSend &request_send) {
  struct WriteBuffer *write_buffer = AppendSendbuffer_(sock_srv, sock.low, request_send, sizeof(*write_buffer));
  sock.wb_size += write_buffer->sz;
}

static int TriggerWrite(SocketServer &sock_srv, struct RequestSend &request_send,
                        struct SocketMessage &socket_message) {
  int id = request_send.id;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) return -1;
  if (EnableWrite(sock_srv, *sock, true)) {
    return ReportError(*sock, socket_message, "enable write failed");
  }
  return -1;
}

/*
        When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

        If socket buffer is empty, write to fd directly.
                If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
        Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
static int SendSocket(SocketServer &sock_srv, struct RequestSend &request_send, struct SocketMessage &socket_message,
                      int priority, const uint8_t *udp_address) {
  int id = request_send.id;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  struct SendObject send_object;
  send_object.buffer = request_send.buffer;
  send_object.sz = request_send.sz;
  uint8_t type = std::atomic_load(&sock->type);
  if (type == kSocketTypeInvalid || sock->id != id || type == kSocketTypeHalfCloseWrite ||
      type == kSocketTypeIPaccept || sock->closing) {
    delete request_send.buffer;
    return -1;
  }
  if (type == kSocketTypePlisten || type == kSocketTypeListen) {
    SkynetError(-1, "socket-server: write to listen fd " + std::to_string(id));
    delete request_send.buffer;
    return -1;
  }
  if (SendBufferEmpty(*sock)) {
    if (sock->protocol == Protocol::TCP) {
      AppendSendbuffer(sock_srv, *sock, request_send);  // add to high priority list, even priority == PRIORITY_LOW
    } else {
      // udp
      if (udp_address == nullptr) {
        udp_address = std::get<uint8_t[kUdpAddressSize]>(sock->p);
      }
      union kSockaddrAll sa;
      socklen_t sasz = UDPSocketAddress(*sock, udp_address, sa);
      if (sasz == 0) {
        // udp type mismatch, just drop it.
        SkynetError(-1, "socket-server: udp socket (" + std::to_string(id) + ") type mismatch.");
        delete request_send.buffer;
        return -1;
      }
      int n = sendto(sock->fd, send_object.buffer, send_object.sz, 0, &sa.s, sasz);
      if (n != send_object.sz) {
        AppendSendbufferUdp(sock_srv, *sock, priority, request_send, udp_address);
      } else {
        StatWrite(sock_srv, *sock, n);
        delete request_send.buffer;
        return -1;
      }
    }
    if (EnableWrite(sock_srv, *sock, true)) {
      return ReportError(*sock, socket_message, "enable write failed");
    }
  } else {
    if (sock->protocol == Protocol::TCP) {
      if (priority == PRIORITY_LOW) {
        AppendSendbufferLow(sock_srv, *sock, request_send);
      } else {
        AppendSendbuffer(sock_srv, *sock, request_send);
      }
    } else {
      if (udp_address == nullptr) {
        udp_address = std::get<uint8_t[kUdpAddressSize]>(sock->p);
      }
      AppendSendbufferUdp(sock_srv, *sock, priority, request_send, udp_address);
    }
  }
  if (sock->wb_size >= WARNING_SIZE && sock->wb_size >= sock->warn_size) {
    sock->warn_size = sock->warn_size == 0 ? WARNING_SIZE * 2 : sock->warn_size * 2;
    socket_message.opaque = sock->opaque;
    socket_message.id = sock->id;
    socket_message.ud = sock->wb_size % 1024 == 0 ? sock->wb_size / 1024 : sock->wb_size / 1024 + 1;
    socket_message.data = nullptr;
    return kSocketWarning;
  }
  return -1;
}

static int ListenSocket(SocketServer &sock_srv, struct RequestListen &request_listen,
                        struct SocketMessage &socket_message) {
  int id = request_listen.id;
  int listen_fd = request_listen.fd;
  struct Socket *sock = NewFd(sock_srv, id, listen_fd, Protocol::TCP, request_listen.opaque, false);
  if (sock == nullptr) {
    close(listen_fd);
    socket_message.opaque = request_listen.opaque;
    socket_message.id = id;
    socket_message.ud = 0;
    socket_message.data = "reach skynet socket number limit";
    sock_srv.slot_[HASH_ID(id)].type = kSocketTypeInvalid;

    return kSocketErr;
  }
  std::atomic_store(&sock->type, kSocketTypePlisten);
  socket_message.opaque = request_listen.opaque;
  socket_message.id = id;
  socket_message.ud = 0;
  socket_message.data = "listen";

  union kSockaddrAll u;
  socklen_t slen = sizeof(u);
  if (getsockname(listen_fd, &u.s, &slen) == 0) {
    void *sin_addr = (u.s.sa_family == AF_INET) ? (void *)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
    if (inet_ntop(u.s.sa_family, sin_addr, sock_srv.buffer_, sizeof(sock_srv.buffer_)) == 0) {
      socket_message.data = strerror(errno);
      return kSocketErr;
    }
    int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
    socket_message.data = sock_srv.buffer_;
    socket_message.ud = sin_port;
  } else {
    socket_message.data = strerror(errno);
    return kSocketErr;
  }

  return kSocketOpen;
}

static inline int NomoreSendingData(struct Socket &sock) {
  return (SendBufferEmpty(sock) && sock.dw_buffer == NULL && (std::atomic_load(&sock.sending) & 0xffff) == 0) ||
         (std::atomic_load(&sock.type) == kSocketTypeHalfCloseWrite);
}

static void CloseRead(SocketServer &sock_srv, struct Socket &sock, struct SocketMessage &socket_message) {
  // Don't read socket later
  std::atomic_store(&sock.type, kSocketTypeHalfCloseRead);
  EnableRead(sock_srv, sock, false);
  shutdown(sock.fd, SHUT_RD);
  socket_message.id = sock.id;
  socket_message.ud = 0;
  socket_message.data = nullptr;
  socket_message.opaque = sock.opaque;
}

static inline int HalfcloseRead(struct Socket &sock) {
  return std::atomic_load(&sock.type) == kSocketTypeHalfCloseRead;
}

// kSocketClose can be raised (only once) in one of two conditions.
// See https://github.com/cloudwu/skynet/issues/1346 for more discussion.
// 1. close socket by self, See CloseSocket()
// 2. recv 0 or eof event (close socket by remote), See ForwardMessageTCP()
// It's able to write data after kSocketClose (In condition 2), but if remote is closed, SOCKET_ERR may raised.
static int CloseSocket(SocketServer &sock_srv, struct RequestClose &request_close,
                       struct SocketMessage &socket_message) {
  int id = request_close.id;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    // The socket is closed, ignore
    return -1;
  }
  struct SocketLock socket_lock;
  SocketLockInit(*sock, socket_lock);

  int shutdown_read = HalfcloseRead(*sock);

  if (request_close.shutdown || NomoreSendingData(*sock)) {
    // If socket is kSocketTypeHalfCloseRead, Do not raise kSocketClose again.
    int r = shutdown_read ? -1 : kSocketClose;
    ForceClose(sock_srv, *sock, socket_lock, socket_message);
    return r;
  }
  sock->closing = true;
  if (!shutdown_read) {
    // don't read socket after socket.close()
    CloseRead(sock_srv, *sock, socket_message);
    return kSocketClose;
  }
  // recv 0 before (socket is kSocketTypeHalfCloseRead) and waiting for sending data out.
  return -1;
}

static int BindSocket(SocketServer &sock_srv, struct RequestBind &request_bind, struct SocketMessage &socket_message) {
  int id = request_bind.id;
  socket_message.id = id;
  socket_message.opaque = request_bind.opaque;
  socket_message.ud = 0;
  struct Socket *sock = NewFd(sock_srv, id, request_bind.fd, Protocol::TCP, request_bind.opaque, true);
  if (sock == NULL) {
    socket_message.data = "reach skynet socket number limit";
    return kSocketErr;
  }
  EpollNonblocking(request_bind.fd);
  std::atomic_store(&sock->type, kSocketTypeBind);
  socket_message.data = "binding";
  return kSocketOpen;
}

static int ResumeSocket(SocketServer &sock_srv, struct RequestResumepause &request_resumepause,
                        struct SocketMessage &socket_message) {
  int id = request_resumepause.id;
  socket_message.id = id;
  socket_message.opaque = request_resumepause.opaque;
  socket_message.ud = 0;
  socket_message.data = nullptr;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    socket_message.data = "invalid socket";
    return kSocketErr;
  }
  if (HalfcloseRead(*sock)) {
    // The closing socket may be in transit, so raise an error. See https://github.com/cloudwu/skynet/issues/1374
    socket_message.data = "socket closed";
    return kSocketErr;
  }
  struct SocketLock socket_lock;

  SocketLockInit(*sock, socket_lock);
  if (EnableRead(sock_srv, *sock, true)) {
    socket_message.data = "enable read failed";
    return kSocketErr;
  }
  uint8_t type = std::atomic_load(&sock->type);
  if (type == kSocketTypeIPaccept || type == kSocketTypePlisten) {
    std::atomic_store(&sock->type, (type == kSocketTypeIPaccept) ? kSocketTypeConnected : kSocketTypeListen);
    sock->opaque = request_resumepause.opaque;
    socket_message.data = "start";
    return kSocketOpen;
  } else if (type == kSocketTypeConnected) {
    // todo: maybe we should send a message SOCKET_TRANSFER to s.opaque
    sock->opaque = request_resumepause.opaque;
    socket_message.data = "transfer";
    return kSocketOpen;
  }
  // if s->type == kSocketTypeHalfCloseWrite , SOCKET_CLOSE message will send later
  return -1;
}

static int PauseSocket(SocketServer &sock_srv, struct RequestResumepause &request_resumepause,
                       struct SocketMessage &socket_message) {
  int id = request_resumepause.id;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    return -1;
  }
  if (EnableRead(sock_srv, *sock, false)) {
    return ReportError(*sock, socket_message, "enable read failed");
  }
  return -1;
}

static void SetOptSocket(SocketServer &sock_srv, struct RequestSetOpt &request_setopt) {
  int id = request_setopt.id;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    return;
  }
  int v = request_setopt.value;
  setsockopt(sock->fd, IPPROTO_TCP, request_setopt.what, &v, sizeof(v));
}

static void BlockReadpipe(int pipefd, void *buffer, int sz) {
  while (true) {
    int n = read(pipefd, buffer, sz);
    if (n < 0) {
      if (errno == EINTR) continue;
      SkynetError(-1, "socket-server : read pipe error " + std::string(strerror(errno)));
      return;
    }
    // must atomic read from a pipe
    assert(n == sz);
    return;
  }
}

/*判断是否有其他线程通过管道，向socket线程发送消息的标记变量*/
static bool HasCmd(SocketServer &sock_srv) {
  struct timeval tv = {0, 0};
  int retval;
  FD_SET(sock_srv.recvctrl_fd_, &sock_srv.rfds_);
  retval = select(sock_srv.recvctrl_fd_ + 1, &sock_srv.rfds_, nullptr, nullptr, &tv);
  if (retval == 1) {
    return 1;
  }
  return 0;
}

static void AddUDPSocket(SocketServer &sock_srv, struct RequestUDP &request_udp) {
  int id = request_udp.id;

  Protocol protocol;
  if (request_udp.family == AF_INET6) {
    protocol = Protocol::UDPv6;
  } else {
    protocol = Protocol::UDP;
  }
  struct Socket *sock = NewFd(sock_srv, id, request_udp.fd, protocol, request_udp.opaque, true);
  if (sock == nullptr) {
    close(request_udp.fd);
    sock_srv.slot_[HASH_ID(id)].type = kSocketTypeInvalid;
    return;
  }
  std::atomic_store(&sock->type, kSocketTypeConnected);
  auto &udp_address = std::get<uint8_t[kUdpAddressSize]>(sock->p);
  std::fill(std::begin(udp_address), std::end(udp_address), 0);
}

static int SetUDPAddress(SocketServer &sock_srv, struct RequestSetUDP &request_setudp,
                         struct SocketMessage &socket_message) {
  int id = request_setudp.id;
  struct Socket *sock = &sock_srv.slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    return -1;
  }
  Protocol protocol = static_cast<Protocol>(request_setudp.address[0]);
  if (protocol != sock->protocol) {
    // protocol mismatch
    return ReportError(*sock, socket_message, "protocol mismatch");
  }
  auto &p = std::get<uint8_t[kUdpAddressSize]>(sock->p);
  if (protocol == Protocol::UDP) {
    std::copy(request_setudp.address, request_setudp.address + 1 + 2 + 4, p);  // 1 type, 2 port, 4 ipv4
  } else {
    std::copy(request_setudp.address, request_setudp.address + 1 + 2 + 16, p);  // 1 type, 2 port, 16 ipv6
  }
  std::atomic_fetch_sub(&sock->udpconnecting, decltype((&sock->udpconnecting)->load())(1));
  return -1;
}

static inline void IncSendingRef(struct Socket &s, int id) {
  if (s.protocol != Protocol::TCP) return;
  while (true) {
    unsigned long sending = std::atomic_load(&s.sending);
    if ((sending >> 16) == ID_TAG16(id)) {
      if ((sending & 0xffff) == 0xffff) {
        // s.sending may overflow (rarely), so busy waiting here for socket thread dec it. see issue #794
        continue;
      }
      // inc sending only matching the same socket id
      if (std::atomic_compare_exchange_weak(&s.sending, &sending, sending + 1)) return;
      // atom inc failed, retry
    } else {
      // socket id changed, just return
      return;
    }
  }
}

static inline void DecSendingRef(SocketServer &sock_srv, int id) {
  struct Socket *s = &sock_srv.slot_[HASH_ID(id)];
  // Notice: udp may inc sending while type == kSocketTypeReserve
  if (s->id == id && s->protocol == Protocol::TCP) {
    assert((std::atomic_load(&s->sending) & 0xffff) != 0);
    std::atomic_fetch_sub(&s->sending, decltype((&s->sending)->load())(1));
  }
}

// return type
static int CtrlCmd(SocketServer &sock_srv, struct SocketMessage &socket_message) {
  int fd = sock_srv.recvctrl_fd_;
  // the length of message is one byte, so 256+8 buffer size is enough.
  uint8_t buffer[256];
  uint8_t header[2];
  BlockReadpipe(fd, header, sizeof(header));
  int type = header[0];
  int len = header[1];
  BlockReadpipe(fd, buffer, len);
  // ctrl command only exist in local fd, so don't worry about endian.
  switch (type) {
    case 'R':
      return ResumeSocket(sock_srv, *(struct RequestResumepause *)buffer, socket_message);
    case 'S':
      return PauseSocket(sock_srv, *(struct RequestResumepause *)buffer, socket_message);
    case 'B':
      return BindSocket(sock_srv, *(struct RequestBind *)buffer, socket_message);
    case 'L':
      return ListenSocket(sock_srv, *(struct RequestListen *)buffer, socket_message);
    case 'K':
      return CloseSocket(sock_srv, *(struct RequestClose *)buffer, socket_message);
    case 'O':
      return OpenSocket(sock_srv, *(struct RequestOpen *)buffer, socket_message);
    case 'X':
      socket_message.opaque = 0;
      socket_message.id = 0;
      socket_message.ud = 0;
      socket_message.data = nullptr;
      return kSocketExit;
    case 'W':
      return TriggerWrite(sock_srv, *(struct RequestSend *)buffer, socket_message);
    case 'D':
    case 'P': {
      int priority = (type == 'D') ? PRIORITY_HIGH : PRIORITY_LOW;
      struct RequestSend *request = (struct RequestSend *)buffer;
      int ret = SendSocket(sock_srv, *request, socket_message, priority, NULL);
      DecSendingRef(sock_srv, request->id);
      return ret;
    }
    case 'A': {
      struct RequestSendUDP *rsu = (struct RequestSendUDP *)buffer;
      return SendSocket(sock_srv, rsu->send, socket_message, PRIORITY_HIGH, rsu->address);
    }
    case 'C':
      return SetUDPAddress(sock_srv, *(struct RequestSetUDP *)buffer, socket_message);
    case 'T':
      SetOptSocket(sock_srv, *(struct RequestSetOpt *)buffer);
      return -1;
    case 'U':
      AddUDPSocket(sock_srv, *(struct RequestUDP *)buffer);
      return -1;
    default:
      SkynetError(-1, "socket-server: Unknown ctrl " + std::to_string(type));
      return -1;
  };

  return -1;
}

// return -1 (ignore) when error
static int ForwardMessageTCP(SocketServer &sock_srv, struct Socket &sock, struct SocketLock &socket_lock,
                             struct SocketMessage &socket_message) {
  int sz = std::get<int>(sock.p);
  char *buffer;  // = MALLOC(sz);
  int n = (int)read(sock.fd, buffer, sz);
  if (n < 0) {
    // FREE(buffer);
    switch (errno) {
      case EINTR:
      case AGAIN_WOULDBLOCK:
        break;
      default:
        return ReportError(sock, socket_message, strerror(errno));
    }
    return -1;
  }
  if (n == 0) {
    delete buffer;
    if (sock.closing) {
      // Rare case : if s.closing is true, reading event is disable, and SOCKET_CLOSE is raised.
      if (NomoreSendingData(sock)) {
        ForceClose(sock_srv, sock, socket_lock, socket_message);
      }
      return -1;
    }
    int t = std::atomic_load(&sock.type);
    if (t == kSocketTypeHalfCloseRead) {
      // Rare case : Already shutdown read.
      return -1;
    }
    if (t == kSocketTypeHalfCloseWrite) {
      // Remote shutdown read (write error) before.
      ForceClose(sock_srv, sock, socket_lock, socket_message);
    } else {
      CloseRead(sock_srv, sock, socket_message);
    }
    return kSocketClose;
  }

  if (HalfcloseRead(sock)) {
    // discard recv data (Rare case : if socket is HALFCLOSE_READ, reading event is disable.)
    // FREE(buffer);
    return -1;
  }

  StatRead(sock_srv, sock, n);

  socket_message.opaque = sock.opaque;
  socket_message.id = sock.id;
  socket_message.ud = n;
  socket_message.data = buffer;

  if (n == sz) {
    sock.p = std::get<int>(sock.p) * 2;
    return kSocketMore;
  } else if (sz > MIN_READ_BUFFER && n * 2 < sz) {
    sock.p = std::get<int>(sock.p) / 2;
  }

  return kSocketData;
}

static int GenUDPAddress(Protocol protocol, union kSockaddrAll &sa, uint8_t *udp_address) {
  int addrsz = 1;
  udp_address[0] = (uint8_t)protocol;
  if (protocol == Protocol::UDP) {
    memcpy(udp_address + addrsz, &sa.v4.sin_port, sizeof(sa.v4.sin_port));
    addrsz += sizeof(sa.v4.sin_port);
    memcpy(udp_address + addrsz, &sa.v4.sin_addr, sizeof(sa.v4.sin_addr));
    addrsz += sizeof(sa.v4.sin_addr);
  } else {
    memcpy(udp_address + addrsz, &sa.v6.sin6_port, sizeof(sa.v6.sin6_port));
    addrsz += sizeof(sa.v6.sin6_port);
    memcpy(udp_address + addrsz, &sa.v6.sin6_addr, sizeof(sa.v6.sin6_addr));
    addrsz += sizeof(sa.v6.sin6_addr);
  }
  return addrsz;
}

static int ForwardMessageUDP(SocketServer &sock_srv, struct Socket &sock, struct SocketLock &socket_lock,
                             struct SocketMessage &socket_message) {
  union kSockaddrAll sa;
  socklen_t slen = sizeof(sa);
  int n = recvfrom(sock.fd, sock_srv.udpbuffer_, MAX_UDP_PACKAGE, 0, &sa.s, &slen);
  if (n < 0) {
    switch (errno) {
      case EINTR:
      case AGAIN_WOULDBLOCK:
        return -1;
    }
    int error = errno;
    // close when error
    ForceClose(sock_srv, sock, socket_lock, socket_message);
    socket_message.data = strerror(error);
    return kSocketErr;
  }
  StatRead(sock_srv, sock, n);

  uint8_t *data;
  if (slen == sizeof(sa.v4)) {
    if (sock.protocol != Protocol::UDP) return -1;
    // data = MALLOC(n + 1 + 2 + 4);
    GenUDPAddress(Protocol::UDP, sa, data + n);
  } else {
    if (sock.protocol != Protocol::UDPv6) return -1;
    // data = MALLOC(n + 1 + 2 + 16);
    GenUDPAddress(Protocol::UDPv6, sa, data + n);
  }
  memcpy(data, sock_srv.udpbuffer_, n);

  socket_message.opaque = sock.opaque;
  socket_message.id = sock.id;
  socket_message.ud = n;
  socket_message.data = (char *)data;

  return kSocketUDP;
}

static int ReportConnect(SocketServer &sock_srv, struct Socket &sock, struct SocketLock &socket_lock,
                         struct SocketMessage &socket_message) {
  int error;
  socklen_t len = sizeof(error);
  int code = getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, &error, &len);
  if (code < 0 || error) {
    error = code < 0 ? errno : error;
    ForceClose(sock_srv, sock, socket_lock, socket_message);
    socket_message.data = strerror(error);
    return kSocketErr;
  } else {
    std::atomic_store(&sock.type, kSocketTypeConnected);
    socket_message.opaque = sock.opaque;
    socket_message.id = sock.id;
    socket_message.ud = 0;
    if (NomoreSendingData(sock)) {
      if (EnableWrite(sock_srv, sock, false)) {
        ForceClose(sock_srv, sock, socket_lock, socket_message);
        socket_message.data = "disable write failed";
        return kSocketErr;
      }
    }
    union kSockaddrAll u;
    socklen_t slen = sizeof(u);
    if (getpeername(sock.fd, &u.s, &slen) == 0) {
      void *sin_addr = (u.s.sa_family == AF_INET) ? (void *)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
      if (inet_ntop(u.s.sa_family, sin_addr, sock_srv.buffer_, sizeof(sock_srv.buffer_))) {
        socket_message.data = sock_srv.buffer_;
        return kSocketOpen;
      }
    }
    socket_message.data = nullptr;
    return kSocketOpen;
  }
}

static int GetName(union kSockaddrAll &sa, char *buffer, size_t sz) {
  char tmp[INET6_ADDRSTRLEN];
  void *sin_addr = (sa.s.sa_family == AF_INET) ? (void *)&sa.v4.sin_addr : (void *)&sa.v6.sin6_addr;
  if (inet_ntop(sa.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
    int sin_port = ntohs((sa.s.sa_family == AF_INET) ? sa.v4.sin_port : sa.v6.sin6_port);
    snprintf(buffer, sz, "%s:%d", tmp, sin_port);
    return 1;
  } else {
    buffer[0] = '\0';
    return 0;
  }
}

// return 0 when failed, or -1 when file limit
static int ReportAccept(SocketServer &sock_srv, struct Socket &sock, struct SocketMessage &socket_message) {
  union kSockaddrAll sa;
  socklen_t len = sizeof(sa);
  int client_fd = accept(sock.fd, &sa.s, &len);
  if (client_fd < 0) {
    if (errno == EMFILE || errno == ENFILE) {
      socket_message.opaque = sock.opaque;
      socket_message.id = sock.id;
      socket_message.ud = 0;
      socket_message.data = strerror(errno);

      // See
      // https://stackoverflow.com/questions/47179793/how-to-gracefully-handle-accept-giving-emfile-and-close-the-connection
      if (sock_srv.reserve_fd_ >= 0) {
        close(sock_srv.reserve_fd_);
        client_fd = accept(sock.fd, &sa.s, &len);
        if (client_fd >= 0) {
          close(client_fd);
        }
        sock_srv.reserve_fd_ = dup(1);
      }
      return -1;
    } else {
      return 0;
    }
  }
  int id = ReserveId(sock_srv);
  if (id < 0) {
    close(client_fd);
    return 0;
  }
  SocketKeepalive(client_fd);
  EpollNonblocking(client_fd);
  struct Socket *new_sock = NewFd(sock_srv, id, client_fd, Protocol::TCP, sock.opaque, false);
  if (new_sock == nullptr) {
    close(client_fd);
    return 0;
  }
  // accept new one connection
  StatRead(sock_srv, sock, 1);

  std::atomic_store(&new_sock->type, kSocketTypeIPaccept);
  socket_message.opaque = sock.opaque;
  socket_message.id = sock.id;
  socket_message.ud = id;
  socket_message.data = nullptr;

  if (GetName(sa, sock_srv.buffer_, sizeof(sock_srv.buffer_))) {
    socket_message.data = sock_srv.buffer_;
  }

  return 1;
}

static inline void ClearClosedEvent(SocketServer &sock_srv, struct SocketMessage &socket_message, int type) {
  if (type == kSocketClose || type == kSocketErr) {
    int id = socket_message.id;
    int i;
    for (i = sock_srv.event_index_; i < sock_srv.event_n_; i++) {
      struct Event *event = &sock_srv.ev_[i];
      struct Socket *sock = (struct Socket *)event->s;
      if (sock) {
        if (SocketInvalid(*sock, id) && sock->id == id) {
          event->s = NULL;
          break;
        }
      }
    }
  }
}

static void SendRequest(SocketServer &sock_srv, struct RequestPackage &request_package, char type, int len) {
  request_package.header[6] = (uint8_t)type;
  request_package.header[7] = (uint8_t)len;
  const void *req = &request_package + offsetof(struct RequestPackage, header[6]);
  while (true) {
    ssize_t n = write(sock_srv.sendctrl_fd_, req, len + 2);
    if (n < 0) {
      if (errno != EINTR) {
        SkynetError(-1, "socket-server : send ctrl command error " + std::string(strerror(errno)));
      }
      continue;
    }
    assert(n == len + 2);
    return;
  }
}

static int OpenRequest(SocketServer &sock_srv, struct RequestPackage &request_package, uintptr_t opaque,
                       const char *addr, int port) {
  int len = strlen(addr);
  if (len + sizeof(request_package.u.open) >= 256) {
    SkynetError(-1, "socket-server : Invalid addr " + std::string(addr));
    return -1;
  }
  int id = ReserveId(sock_srv);
  if (id < 0) return -1;
  request_package.u.open.opaque = opaque;
  request_package.u.open.id = id;
  request_package.u.open.port = port;
  memcpy(request_package.u.open.host, addr, len);
  request_package.u.open.host[len] = '\0';

  return len;
}

static inline int CanDirectWrite(struct Socket &sock, int id) {
  return sock.id == id && NomoreSendingData(sock) && std::atomic_load(&sock.type) == kSocketTypeConnected &&
         std::atomic_load(&sock.udpconnecting) == 0;
}

// return -1 means failed
// or return AF_INET or AF_INET6
static int DoBind(const char *host, int port, int protocol, int *family) {
  int fd;
  int status;
  int reuse = 1;
  struct addrinfo ai_hints;
  struct addrinfo *ai_list = NULL;
  char portstr[16];
  if (host == NULL || host[0] == 0) {
    host = "0.0.0.0";  // INADDR_ANY
  }
  sprintf(portstr, "%d", port);
  memset(&ai_hints, 0, sizeof(ai_hints));
  ai_hints.ai_family = AF_UNSPEC;
  if (protocol == IPPROTO_TCP) {
    ai_hints.ai_socktype = SOCK_STREAM;
  } else {
    assert(protocol == IPPROTO_UDP);
    ai_hints.ai_socktype = SOCK_DGRAM;
  }
  ai_hints.ai_protocol = protocol;

  status = getaddrinfo(host, portstr, &ai_hints, &ai_list);
  if (status != 0) {
    return -1;
  }
  *family = ai_list->ai_family;
  fd = socket(*family, ai_list->ai_socktype, 0);
  if (fd < 0) {
    freeaddrinfo(ai_list);
    return -1;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int)) == -1) {
    close(fd);
    freeaddrinfo(ai_list);
    return -1;
  }
  status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
  if (status != 0) {
    close(fd);
    freeaddrinfo(ai_list);
    return -1;
  }

  freeaddrinfo(ai_list);
  return fd;
}

static int DoListen(const char *host, int port, int backlog) {
  int family = 0;
  int listen_fd = DoBind(host, port, IPPROTO_TCP, &family);
  if (listen_fd < 0) {
    return -1;
  }
  if (listen(listen_fd, backlog) == -1) {
    close(listen_fd);
    return -1;
  }
  return listen_fd;
}

/**************************SocketServer******************************/

SocketServer::SocketServer(uint64_t time, poll_fd efd, int fd[2]) {
  this->time_ = time;
  this->event_fd_ = efd;
  this->recvctrl_fd_ = fd[0];
  this->sendctrl_fd_ = fd[1];
  this->checkctrl_ = 1;
  this->reserve_fd_ = dup(1);  // reserve an extra fd for EMFILE

  for (int i = 0; i < MAX_SOCKET; i++) {
    struct Socket *sock = &this->slot_[i];
    std::atomic_init(&sock->type, kSocketTypeInvalid);
    ClearWriteBufferList(sock->high);
    ClearWriteBufferList(sock->low);
    spinlock_init(&sock->dw_lock);
  }
  std::atomic_init(&this->alloc_id_, 0);
  this->event_n_ = 0;
  this->event_index_ = 0;
  FD_ZERO(&this->rfds_);
  assert(this->recvctrl_fd_ < FD_SETSIZE);
}

SocketServer::~SocketServer() {
  int i;
  struct SocketMessage dummy;
  for (i = 0; i < MAX_SOCKET; i++) {
    struct Socket *sock = &this->slot_[i];
    struct SocketLock socket_lock;
    SocketLockInit(*sock, socket_lock);
    if (std::atomic_load(&sock->type) != kSocketTypeReserve) {
      ForceClose(*this, *sock, socket_lock, dummy);
    }
    spinlock_destroy(&sock->dw_lock);
  }
  close(this->sendctrl_fd_);
  close(this->recvctrl_fd_);
  EpollClose(this->event_fd_);
  if (this->reserve_fd_ >= 0) close(this->reserve_fd_);
}

void SocketServer::UpdateTime(uint64_t time) { this->time_ = time; }

int SocketServer::Poll(struct SocketMessage &socket_message, int *more) {
  while (true) {
    if (this->checkctrl_) {
      // 服务间通信检查
      if (HasCmd(*this)) {
        int type = CtrlCmd(*this, socket_message);
        if (type != -1) {
          ClearClosedEvent(*this, socket_message, type);
          return type;
        } else
          continue;
      } else {
        this->checkctrl_ = 0;
      }
    }

    if (this->event_index_ == this->event_n_) {
      this->event_n_ = EpollWait(this->event_fd_, this->ev_);
      this->checkctrl_ = 1;
      if (more) {
        *more = 0;
      }
      this->event_index_ = 0;
      if (this->event_n_ <= 0) {
        this->event_n_ = 0;
        int err = errno;
        if (err != EINTR) {
          SkynetError(-1, "socket-server: " + std::string(strerror(err)));
        }
        continue;
      }
    }
    struct Event *event = &this->ev_[this->event_index_];
    this->event_index_++;
    struct Socket *sock = event->s;
    if (sock == nullptr) {
      // dispatch pipe message at beginning
      continue;
    }
    struct SocketLock socket_lock;
    SocketLockInit(*sock, socket_lock);
    switch (std::atomic_load(&sock->type)) {
      case kSocketTypeConnecting:
        return ReportConnect(*this, *sock, socket_lock, socket_message);
      case kSocketTypeListen: {
        int ok = ReportAccept(*this, *sock, socket_message);
        if (ok > 0) {
          return kSocketAccept;
        }
        if (ok < 0) {
          return kSocketErr;
        }
        // when ok == 0, retry
        break;
      }
      case kSocketTypeInvalid:
        SkynetError(-1, "socket-server: invalid socket");
        break;
      default:
        if (event->read) {
          int type;
          if (sock->protocol == Protocol::TCP) {
            type = ForwardMessageTCP(*this, *sock, socket_lock, socket_message);
            if (type == kSocketMore) {
              --this->event_index_;
              return kSocketData;
            }
          } else {
            type = ForwardMessageUDP(*this, *sock, socket_lock, socket_message);
            if (type == kSocketUDP) {
              // try read again
              --this->event_index_;
              return kSocketUDP;
            }
          }
          if (event->write && type != kSocketClose && type != kSocketErr) {
            // Try to dispatch write message next step if write flag set.
            event->read = false;
            --this->event_index_;
          }
          if (type == -1) break;
          return type;
        }
        if (event->write) {
          int type = SendBuffer(*this, *sock, socket_lock, socket_message);
          if (type == -1) break;
          return type;
        }
        if (event->error) {
          int error;
          socklen_t len = sizeof(error);
          int code = getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &error, &len);
          const char *err = NULL;
          if (code < 0) {
            err = strerror(errno);
          } else if (error != 0) {
            err = strerror(error);
          } else {
            err = "Unknown error";
          }
          return ReportError(*sock, socket_message, err);
        }
        if (event->eof) {
          // For epoll (at least), FIN packets are exchanged both ways.
          // See: https://stackoverflow.com/questions/52976152/tcp-when-is-epollhup-generated
          int halfclose = HalfcloseRead(*sock);
          ForceClose(*this, *sock, socket_lock, socket_message);
          if (!halfclose) {
            return kSocketClose;
          }
        }
        break;
    }
  }
}

void SocketServer::Exit() {
  struct RequestPackage request;
  SendRequest(*this, request, 'X', 0);
}

int SocketServer::Connect(uintptr_t opaque, const char *addr, int port) {
  struct RequestPackage request_package;
  int len = OpenRequest(*this, request_package, opaque, addr, port);
  if (len < 0) return -1;
  SendRequest(*this, request_package, 'O', sizeof(request_package.u.open) + len);
  return request_package.u.open.id;
}

// return -1 when error, 0 when success
int SocketServer::Send(struct Sendbuffer &socket_sendbuffer) {
  int id = socket_sendbuffer.id;
  struct Socket *sock = &this->slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id) || sock->closing) {
    FreeBuffer(*this, socket_sendbuffer);
    return -1;
  }

  struct SocketLock socket_lock;
  SocketLockInit(*sock, socket_lock);

  if (CanDirectWrite(*sock, id) && SocketTrylock(socket_lock)) {
    // may be we can send directly, double check
    if (CanDirectWrite(*sock, id)) {
      // send directly
      struct SendObject send_object;
      SendObjectInitFromSendbuffer(send_object, socket_sendbuffer);
      ssize_t n;
      if (sock->protocol == Protocol::TCP) {
        n = write(sock->fd, send_object.buffer, send_object.sz);
      } else {
        union kSockaddrAll sa;
        socklen_t sasz = UDPSocketAddress(*sock, std::get<uint8_t[kUdpAddressSize]>(sock->p), sa);
        if (sasz == 0) {
          SkynetError(-1, "socket-server : set udp (" + std::to_string(id) + ") address first.");
          SocketUnlock(socket_lock);
          delete socket_sendbuffer.buffer;
          return -1;
        }
        n = sendto(sock->fd, send_object.buffer, send_object.sz, 0, &sa.s, sasz);
      }
      if (n < 0) {
        // ignore error, let socket thread try again
        n = 0;
      }
      StatWrite(*this, *sock, n);
      if (n == send_object.sz) {
        // write done
        SocketUnlock(socket_lock);
        delete socket_sendbuffer.buffer;
        return 0;
      }
      // write failed, put buffer into sock->dw_* , and let socket thread send it. see SendBuffer()
      sock->dw_buffer = CloneBuffer(socket_sendbuffer, &sock->dw_size);
      sock->dw_offset = n;

      SocketUnlock(socket_lock);

      struct RequestPackage request_package;
      request_package.u.send.id = id;
      request_package.u.send.sz = 0;
      request_package.u.send.buffer = NULL;

      // let socket thread enable write event
      SendRequest(*this, request_package, 'W', sizeof(request_package.u.send));

      return 0;
    }
    SocketUnlock(socket_lock);
  }

  IncSendingRef(*sock, id);

  struct RequestPackage request_package;
  request_package.u.send.id = id;
  request_package.u.send.buffer = CloneBuffer(socket_sendbuffer, &request_package.u.send.sz);

  SendRequest(*this, request_package, 'D', sizeof(request_package.u.send));
  return 0;
}

// return -1 when error, 0 when success
int SocketServer::SendLowpriority(struct Sendbuffer &socket_sendbuffer) {
  int id = socket_sendbuffer.id;

  struct Socket *sock = &this->slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    FreeBuffer(*this, socket_sendbuffer);
    return -1;
  }

  IncSendingRef(*sock, id);

  struct RequestPackage request;
  request.u.send.id = id;
  request.u.send.buffer = CloneBuffer(socket_sendbuffer, &request.u.send.sz);

  SendRequest(*this, request, 'P', sizeof(request.u.send));
  return 0;
}

void SocketServer::Close(uintptr_t opaque, int id) {
  struct RequestPackage request;
  request.u.close.id = id;
  request.u.close.shutdown = 0;
  request.u.close.opaque = opaque;
  SendRequest(*this, request, 'K', sizeof(request.u.close));
}

void SocketServer::Shutdown(uintptr_t opaque, int id) {
  struct RequestPackage request;
  request.u.close.id = id;
  request.u.close.shutdown = 1;
  request.u.close.opaque = opaque;
  SendRequest(*this, request, 'K', sizeof(request.u.close));
}

int SocketServer::Listen(uintptr_t opaque, const char *addr, int port, int backlog) {
  int fd = DoListen(addr, port, backlog);
  if (fd < 0) {
    return -1;
  }
  struct RequestPackage request;
  int id = ReserveId(*this);
  if (id < 0) {
    close(fd);
    return id;
  }
  request.u.listen.opaque = opaque;
  request.u.listen.id = id;
  request.u.listen.fd = fd;
  SendRequest(*this, request, 'L', sizeof(request.u.listen));
  return id;
}

int SocketServer::Bind(uintptr_t opaque, int fd) {
  struct RequestPackage request;
  int id = ReserveId(*this);
  if (id < 0) return -1;
  request.u.bind.opaque = opaque;
  request.u.bind.id = id;
  request.u.bind.fd = fd;
  SendRequest(*this, request, 'B', sizeof(request.u.bind));
  return id;
}

void SocketServer::Start(uintptr_t opaque, int id) {
  struct RequestPackage request;
  request.u.resumepause.id = id;
  request.u.resumepause.opaque = opaque;
  SendRequest(*this, request, 'R', sizeof(request.u.resumepause));
}

void SocketServer::Pause(uintptr_t opaque, int id) {
  struct RequestPackage request;
  request.u.resumepause.id = id;
  request.u.resumepause.opaque = opaque;
  SendRequest(*this, request, 'S', sizeof(request.u.resumepause));
}

void SocketServer::Nodelay(int id) {
  struct RequestPackage request;
  request.u.setopt.id = id;
  request.u.setopt.what = TCP_NODELAY;
  request.u.setopt.value = 1;
  SendRequest(*this, request, 'T', sizeof(request.u.setopt));
}

// UDP

int SocketServer::UDP(uintptr_t opaque, const char *addr, int port) {
  int fd;
  int family;
  if (port != 0 || addr != NULL) {
    // bind
    fd = DoBind(addr, port, IPPROTO_UDP, &family);
    if (fd < 0) {
      return -1;
    }
  } else {
    family = AF_INET;
    fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
      return -1;
    }
  }
  EpollNonblocking(fd);

  int id = ReserveId(*this);
  if (id < 0) {
    close(fd);
    return -1;
  }
  struct RequestPackage request;
  request.u.udp.id = id;
  request.u.udp.fd = fd;
  request.u.udp.opaque = opaque;
  request.u.udp.family = family;

  SendRequest(*this, request, 'U', sizeof(request.u.udp));
  return id;
}

int SocketServer::UDPSend(std::string addr, struct Sendbuffer &socket_sendbuffer) {
  int id = socket_sendbuffer.id;
  struct Socket *sock = &this->slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    FreeBuffer(*this, socket_sendbuffer);
    return -1;
  }

  const uint8_t *udp_address = (const uint8_t *)addr.c_str();
  int addrsz;
  switch (udp_address[0]) {
    case PROTOCOL_UDP:
      addrsz = 1 + 2 + 4;  // 1 type, 2 port, 4 ipv4
      break;
    case PROTOCOL_UDPv6:
      addrsz = 1 + 2 + 16;  // 1 type, 2 port, 16 ipv6
      break;
    default:
      FreeBuffer(*this, socket_sendbuffer);
      return -1;
  }

  struct SocketLock socket_lock;
  SocketLockInit(*sock, socket_lock);

  if (CanDirectWrite(*sock, id) && SocketTrylock(socket_lock)) {
    // may be we can send directly, double check
    if (CanDirectWrite(*sock, id)) {
      // send directly
      struct SendObject send_object;
      SendObjectInitFromSendbuffer(send_object, socket_sendbuffer);
      union kSockaddrAll sa;
      socklen_t sasz = UDPSocketAddress(*sock, udp_address, sa);
      if (sasz == 0) {
        SocketUnlock(socket_lock);
        delete socket_sendbuffer.buffer;
        return -1;
      }
      int n = sendto(sock->fd, send_object.buffer, send_object.sz, 0, &sa.s, sasz);
      if (n >= 0) {
        // sendto succ
        StatWrite(*this, *sock, n);
        SocketUnlock(socket_lock);
        delete socket_sendbuffer.buffer;
        return 0;
      }
    }
    SocketUnlock(socket_lock);
    // let socket thread try again, udp doesn't care the order
  }

  struct RequestPackage request;
  request.u.send_udp.send.id = id;
  request.u.send_udp.send.buffer = CloneBuffer(socket_sendbuffer, &request.u.send_udp.send.sz);

  memcpy(request.u.send_udp.address, udp_address, addrsz);

  SendRequest(*this, request, 'A', sizeof(request.u.send_udp.send) + addrsz);
  return 0;
}

int SocketServer::UDPConnect(int id, const char *addr, int port) {
  struct Socket *sock = &this->slot_[HASH_ID(id)];
  if (SocketInvalid(*sock, id)) {
    return -1;
  }
  struct SocketLock socket_lock;
  SocketLockInit(*sock, socket_lock);
  SocketLock(socket_lock);
  if (SocketInvalid(*sock, id)) {
    SocketUnlock(socket_lock);
    return -1;
  }
  std::atomic_fetch_add(&sock->udpconnecting, decltype((&sock->udpconnecting)->load())(1));
  SocketUnlock(socket_lock);

  int status;
  struct addrinfo ai_hints;
  struct addrinfo *ai_list = NULL;
  char portstr[16];
  sprintf(portstr, "%d", port);
  memset(&ai_hints, 0, sizeof(ai_hints));
  ai_hints.ai_family = AF_UNSPEC;
  ai_hints.ai_socktype = SOCK_DGRAM;
  ai_hints.ai_protocol = IPPROTO_UDP;

  status = getaddrinfo(addr, portstr, &ai_hints, &ai_list);
  if (status != 0) {
    return -1;
  }
  struct RequestPackage request;
  request.u.set_udp.id = id;
  Protocol protocol;

  if (ai_list->ai_family == AF_INET) {
    protocol = Protocol::UDP;
  } else if (ai_list->ai_family == AF_INET6) {
    protocol = Protocol::UDPv6;
  } else {
    freeaddrinfo(ai_list);
    return -1;
  }

  int addrsz = GenUDPAddress(protocol, *(union kSockaddrAll *)ai_list->ai_addr, request.u.set_udp.address);

  freeaddrinfo(ai_list);

  SendRequest(*this, request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) + addrsz);

  return 0;
}

std::vector<int> SocketServer::UdpAddress(struct SocketMessage &socket_message, int *address) {
  // std::string *address = &socket_message.data + socket_message.ud;
  // int type = address[0];
  // switch (type) {
  //   case PROTOCOL_UDP:
  //     *addrsz = 1 + 2 + 4;
  //     break;
  //   case PROTOCOL_UDPv6:
  //     *addrsz = 1 + 2 + 16;
  //     break;
  //   default:
  //     return nullptr;
  // }
  // return address;
  return {};
}

static int QueryInfo(struct Socket &sock, SocketInfo &sock_info) {
  union kSockaddrAll u;
  socklen_t slen = sizeof(u);
  int closing = 0;
  switch (std::atomic_load(&sock.type)) {
    case kSocketTypeBind:
      sock_info.type_ = SOCKET_INFO_BIND;
      sock_info.name[0] = '\0';
      break;
    case kSocketTypeListen:
      sock_info.type_ = SOCKET_INFO_LISTEN;
      if (getsockname(sock.fd, &u.s, &slen) == 0) {
        GetName(u, sock_info.name, sizeof(sock_info.name));
      }
      break;
    case kSocketTypeHalfCloseRead:
    case kSocketTypeHalfCloseWrite:
      closing = 1;
    case kSocketTypeConnected:
      if (sock.protocol == Protocol::TCP) {
        sock_info.type_ = closing ? SOCKET_INFO_CLOSING : SOCKET_INFO_TCP;
        if (getpeername(sock.fd, &u.s, &slen) == 0) {
          GetName(u, sock_info.name, sizeof(sock_info.name));
        }
      } else {
        sock_info.type_ = SOCKET_INFO_UDP;
        if (UDPSocketAddress(sock, std::get<uint8_t[kUdpAddressSize]>(sock.p), u)) {
          GetName(u, sock_info.name, sizeof(sock_info.name));
        }
      }
      break;
    default:
      return 0;
  }
  sock_info.id_ = sock.id;
  sock_info.opaque_ = (uint64_t)sock.opaque;
  sock_info.read_ = sock.stat.read;
  sock_info.write_ = sock.stat.write;
  sock_info.rtime_ = sock.stat.rtime;
  sock_info.wtime_ = sock.stat.wtime;
  sock_info.wbuffer_ = sock.wb_size;
  sock_info.reading_ = sock.reading;
  sock_info.writing_ = sock.writing;

  return 1;
}

SocketInfo *SocketServer::Info() {
  SocketInfo *socket_info = nullptr;
  for (int i = 0; i < MAX_SOCKET; i++) {
    struct Socket *socket = &this->slot_[i];
    int id = socket->id;
    SocketInfo tmp;
    if (QueryInfo(*socket, tmp) && socket->id == id) {
      // socket_server_info may call in different thread, so check socket id again
      socket_info = socket_info->CreateNext(socket_info);
      tmp.next = socket_info->next;
      *socket_info = tmp;
    }
  }
  return socket_info;
}

SocketServer *CreateSocketServer(uint64_t time) {
  int i;
  int pipe_fd[2];
  poll_fd efd = EpollCreate();
  if (EpollInvalid(efd)) {
    SkynetError(-1, "socket-server: create event pool failed.");
    return nullptr;
  }
  if (pipe(pipe_fd)) {
    // 建立管线通道
    EpollClose(efd);
    SkynetError(-1, "socket-server: create socket pair failed.");
    return nullptr;
  }
  if (EpollAdd(efd, pipe_fd[0], nullptr)) {
    // add recvctrl_fd to event poll
    SkynetError(-1, "socket-server: can't add server fd to event pool.");
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    EpollClose(efd);
    return nullptr;
  }
  SocketServer *socket_server = new SocketServer(time, efd, pipe_fd);

  return socket_server;
}