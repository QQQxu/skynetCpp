#pragma once

#include <string>

#include "socket_info.h"
#include "socket_protocol.h"
#include "service.h"

constexpr int kSocketTypeData = 1;
constexpr int kSocketTypeConnect = 2;
constexpr int kSocketTypeClose = 3;
constexpr int kSocketTypeAccept = 4;
constexpr int kSocketTypeError = 5;
constexpr int kSocketTypeUdp = 6;
constexpr int kSocketTypeWarning = 7;

struct SkynetSocketMessage {
	int type;
	int id;
	int ud;
	std::string buffer;
};

void SocketInit();
void SocketExit();
void SocketFree();

int SocketPoll();
void SocketUpdatetime();

int SocketSendBuffer(struct Sendbuffer &socket_sendbuffer);
int SocketSendBufferLowpriority(struct Sendbuffer &socket_sendbuffer);
int SocketListen(Context *ctx, const char *host, int port, int backlog);
int SocketConnect(Context *ctx, const char *host, int port);
int SocketBind(Context *ctx, int fd);
void SocketClose(Context *ctx, int id);
void SocketShutdown(Context *ctx, int id);
void SocketStart(Context *ctx, int id);
void SocketPause(Context *ctx, int id);
void SocketNodelay(Context *ctx, int id);

int SocketUDP(Context *ctx, const char * addr, int port);
int SocketUdpConnect(Context *ctx, int id, const char * addr, int port);
int SocketUdpSendBuffer(Context *ctx, const char * address, struct Sendbuffer *buffer);
std::vector<int> SocketUdpAddress(struct SkynetSocketMessage *, int *addrsz);

SocketInfo * GetSocketInfo();

// legacy APIs

static inline void sendbuffer_init_(struct Sendbuffer *buf, int id, const std::string *buffer, int sz) {
	buf->id = id;
	buf->buffer = buffer;
	if (sz < 0) {
		buf->type = SOCKET_BUFFER_OBJECT;
	} else {
		buf->type = SOCKET_BUFFER_MEMORY;
	}
	buf->sz = (size_t)sz;
}

static inline int skynet_socket_send(Context *ctx, int id, std::string *buffer, int sz) {
	struct Sendbuffer tmp;
	sendbuffer_init_(&tmp, id, buffer, sz);
	return SocketSendBuffer(tmp);
}

static inline int skynet_socket_send_lowpriority(Context *ctx, int id, std::string *buffer, int sz) {
	struct Sendbuffer tmp;
	sendbuffer_init_(&tmp, id, buffer, sz);
	return SocketSendBufferLowpriority(tmp);
}

static inline int skynet_socket_udp_send(Context *ctx, int id, const char * address, const std::string *buffer, int sz) {
	struct Sendbuffer tmp;
	sendbuffer_init_(&tmp, id, buffer, sz);
	return SocketUdpSendBuffer(ctx, address, &tmp);
}
