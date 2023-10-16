#pragma once

#include <netinet/in.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "socket_epoll.h"
#include "socket_info.h"
#include "socket_protocol.h"
#include "spinlock.h"

constexpr int kSocketData = 0;
constexpr int kSocketClose = 1;
constexpr int kSocketOpen = 2;
constexpr int kSocketAccept = 3;
constexpr int kSocketErr = 4;
constexpr int kSocketExit = 5;
constexpr int kSocketUDP = 6;
constexpr int kSocketWarning = 7;
// Only for internal use
constexpr int kSocketRst = 8;
constexpr int kSocketMore = 9;

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
constexpr int kMaxEvent = 64;
#define MIN_READ_BUFFER 64

constexpr int kSocketTypeInvalid = 0;
constexpr int kSocketTypeReserve = 1;
constexpr int kSocketTypePlisten = 2;
constexpr int kSocketTypeListen = 3;
constexpr int kSocketTypeConnecting = 4;  // 正在链接
constexpr int kSocketTypeConnected = 5;
constexpr int kSocketTypeHalfCloseRead = 6;
constexpr int kSocketTypeHalfCloseWrite = 7;
constexpr int kSocketTypeIPaccept = 8;
constexpr int kSocketTypeBind = 9;

#define MAX_SOCKET (1 << MAX_SOCKET_P)

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)
#define ID_TAG16(id) ((id >> MAX_SOCKET_P) & 0xffff)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2
#define PROTOCOL_UNKNOWN 255

#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK \
  EAGAIN:                \
  case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

#define WARNING_SIZE (1024 * 1024)

#define USEROBJECT ((size_t)(-1))

struct WriteBuffer {
  WriteBuffer *next;
  const std::string *buffer;
  char *ptr;
  size_t sz;
};

struct WriteBufferUDP {
  struct WriteBuffer buffer;
  uint8_t udp_address[kUdpAddressSize];
};

struct WriteBufferList {
  struct WriteBuffer *head;
  struct WriteBuffer *tail;
};

struct SocketStat {
  uint64_t rtime;
  uint64_t wtime;
  uint64_t read;
  uint64_t write;
};

enum class Protocol : uint8_t { TCP, UDP, UDPv6, UNKNOWN };

struct Socket {
  uintptr_t opaque;               // 该 socket 关联的服务地址
  WriteBufferList high;           // 高优先级发送队列
  WriteBufferList low;            // 低优先级发送队列
  int64_t wb_size;                // 发送数据大小
  SocketStat stat;                // socket 状态信息
  std::atomic_ulong sending;      // 发送中的数据量
  int fd;                         // socket 文件描述符
  int id;                         // 该 socket 在 socket 池中索引
  std::atomic_int type;           // socket 类型，listen，connecting，connected 等？
  Protocol protocol;              // 协议，TCP or UDP？
  bool reading;                   // 是否正在读取数据
  bool writing;                   // 是否正在写入数据
  bool closing;                   // 是否正在关闭连接
  std::atomic_int udpconnecting;  // UDP 连接状态
  int64_t warn_size;              // 警告阈值大小
  std::variant<int, uint8_t[kUdpAddressSize]> p;
  Spinlock dw_lock;
  int dw_offset;          // 立刻发送缓冲区偏移
  const std::string *dw_buffer;  // 立刻发送缓冲区
  size_t dw_size;         // 立刻发送缓冲区大小
};

union kSockaddrAll {
  struct sockaddr s;
  struct sockaddr_in v4;
  struct sockaddr_in6 v6;
};

struct SendObject {
  const void *buffer;
  size_t sz;
};

struct SocketLock {
  struct Spinlock *lock;
  int count;
};

struct SocketMessage {
  int id;
  uintptr_t opaque;
  int ud;  // for accept, ud is new connection id ; for data, ud is size of data
  std::string data;
};

class SocketServer {
 public:
  SocketServer(uint64_t time, poll_fd efd, int fd[2]);
  ~SocketServer();  // void Release(struct socket_server *);
  void UpdateTime(uint64_t time);
  int Poll(struct SocketMessage &result, int *more);
  void Exit();
  void Close(uintptr_t opaque, int id);
  void Shutdown(uintptr_t opaque, int id);
  void Start(uintptr_t opaque, int id);
  void Pause(uintptr_t opaque, int id);

  // return -1 when error
  int Send(struct Sendbuffer &buffer);
  int SendLowpriority(struct Sendbuffer &buffer);

  // ctrl command below returns id
  int Listen(uintptr_t opaque, const char *addr, int port, int backlog);
  int Connect(uintptr_t opaque, const char *addr, int port);
  int Bind(uintptr_t opaque, int fd);

  // for tcp
  void Nodelay(int id);

  // create an udp socket handle, attach opaque with it . udp socket don't need call socket_server_start to recv message
  // if port != 0, bind the socket . if addr == NULL, bind ipv4 0.0.0.0 . If you want to use ipv6, addr can be "::" and
  // port 0.
  int UDP(uintptr_t opaque, const char *addr, int port);
  // set default dest address, return 0 when success
  int UDPConnect(int id, const char *addr, int port);
  // If the UDPAddress is NULL, use last call socket_server_udp_connect address instead
  // You can also use socket_server_send
  int UDPSend(std::string, struct Sendbuffer &buffer);
  // extract the address of the message, struct SocketMessage * should be SOCKET_UDP
  std::vector<int> UdpAddress(struct SocketMessage &socket_message, int *addrsz);

  SocketInfo *Info();

 public:
  volatile uint64_t time_;
  int reserve_fd_;              // for EMFILE
  int recvctrl_fd_;             // 接收管道消息的文件描述
  int sendctrl_fd_;             // 发送管道消息的文件描述
  int checkctrl_;               // 判断是否有其他线程通过管道，向socket线程发送消息的标记变量
  poll_fd event_fd_;            // epoll实例id
  std::atomic_int alloc_id_;    // 已经分配的socket slot列表id
  int event_n_;                 // 标记本次epoll事件的数量
  int event_index_;             // 下一个未处理的epoll事件索引
  struct Event ev_[kMaxEvent];  // epoll事件列表
  struct Socket slot_[MAX_SOCKET];  // socket 列表
  char buffer_[MAX_INFO];           // 地址信息转成字符串以后，存在这里
  uint8_t udpbuffer_[MAX_UDP_PACKAGE];
  fd_set rfds_;
};

SocketServer *CreateSocketServer(uint64_t time);
