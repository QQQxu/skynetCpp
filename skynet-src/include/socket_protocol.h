#pragma once

#include <cstdint>
#include <memory>
#include <string>

constexpr int kUdpAddressSize = 19;  // ipv6 128bit + port 16bit + 1 byte type

#define SOCKET_BUFFER_MEMORY 0
#define SOCKET_BUFFER_OBJECT 1
#define SOCKET_BUFFER_RAWPOINTER 2

struct Sendbuffer {
  int id;
  int type;
  const std::string *buffer;
  size_t sz;
};


struct RequestOpen {
  int id;
  int port;
  uintptr_t opaque;
  char host[1];
};

struct RequestSend {
  int id;
  size_t sz;
  const std::string *buffer;
};

struct RequestSendUDP {
  struct RequestSend send;
  uint8_t address[kUdpAddressSize];
};

struct RequestSetUDP {
  int id;
  uint8_t address[kUdpAddressSize];
};

struct RequestClose {
  int id;
  int shutdown;
  uintptr_t opaque;
};

struct RequestListen {
  int id;
  int fd;
  uintptr_t opaque;
  char host[1];
};

struct RequestBind {
  int id;
  int fd;
  uintptr_t opaque;
};

struct RequestResumepause {
  int id;
  uintptr_t opaque;
};

struct RequestSetOpt {
  int id;
  int what;
  int value;
};

struct RequestUDP {
  int id;
  int fd;
  int family;
  uintptr_t opaque;
};

/*
        The first byte is TYPE

        S Start socket
        B Bind socket
        L Listen socket
        K Close socket
        O Connect to (Open)
        X Exit
        D Send package (high)
        P Send package (low)
        A Send UDP package
        T Set opt
        U Create UDP socket
        C set udp address
        Q query info
 */

struct RequestPackage {
  uint8_t header[8];  // 6 bytes dummy
  union {
    char buffer[256];
    struct RequestOpen open;
    struct RequestSend send;
    struct RequestSendUDP send_udp;
    struct RequestClose close;
    struct RequestListen listen;
    struct RequestBind bind;
    struct RequestResumepause resumepause;
    struct RequestSetOpt setopt;
    struct RequestUDP udp;
    struct RequestSetUDP set_udp;
  } u;
  uint8_t dummy[256];
};