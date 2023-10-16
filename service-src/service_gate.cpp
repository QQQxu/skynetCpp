#include <assert.h>
#include <error.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "databuffer.h"
#include "hashid.h"
#include "service.h"
#include "skynet.h"
#include "socket.h"
#include "socket_server.h"

constexpr int kBackLog = 128;

struct Connection {
  int id;  // socket id
  uint32_t agent;
  uint32_t client;
  char remote_name[32];
  struct databuffer buffer;
};

struct Gate {
  struct Context *ctx;
  int listen_id;
  uint32_t watchdog;
  uint32_t broker;
  int client_tag;
  int header_size;
  int max_connection;
  struct hashid hash;
  struct Connection *conn;
  // todo: save message pool ptr for release
  struct messagepool mp;
};

extern "C" struct Gate *gate_create(void) {
  struct Gate *gate = new Gate();
  gate->listen_id = -1;
  return gate;
}

extern "C" void gate_release(struct Gate *gate) {
  struct Context *ctx = gate->ctx;
  for (int i = 0; i < gate->max_connection; i++) {
    struct Connection *connection = &gate->conn[i];
    if (connection->id >= 0) {
      SocketClose(ctx, connection->id);
    }
  }
  if (gate->listen_id >= 0) {
    SocketClose(ctx, gate->listen_id);
  }
  messagepool_free(&gate->mp);
  hashid_clear(&gate->hash);
  delete gate;
}

static void _parm(char *msg, int sz, int command_sz) {
  while (command_sz < sz) {
    if (msg[command_sz] != ' ') break;
    ++command_sz;
  }
  int i;
  for (i = command_sz; i < sz; i++) {
    msg[i - command_sz] = msg[i];
  }
  msg[i - command_sz] = '\0';
}

static void _forward_agent(struct Gate *gate, int fd, uint32_t agentaddr, uint32_t clientaddr) {
  int id = hashid_lookup(&gate->hash, fd);
  if (id >= 0) {
    struct Connection *agent = &gate->conn[id];
    agent->agent = agentaddr;
    agent->client = clientaddr;
  }
}

static void _ctrl(struct Gate *gate, const void *msg, int sz) {
  struct Context *ctx = gate->ctx;
  char tmp[sz + 1];
  memcpy(tmp, msg, sz);
  tmp[sz] = '\0';
  char *command = tmp;
  int i;
  if (sz == 0) return;
  for (i = 0; i < sz; i++) {
    if (command[i] == ' ') {
      break;
    }
  }
  if (memcmp(command, "kick", i) == 0) {
    _parm(tmp, sz, i);
    int uid = strtol(command, NULL, 10);
    int id = hashid_lookup(&gate->hash, uid);
    if (id >= 0) {
      SocketClose(ctx, uid);
    }
    return;
  }
  if (memcmp(command, "forward", i) == 0) {
    _parm(tmp, sz, i);
    char *client = tmp;
    char *idstr = strsep(&client, " ");
    if (client == NULL) {
      return;
    }
    int id = strtol(idstr, NULL, 10);
    char *agent = strsep(&client, " ");
    if (client == NULL) {
      return;
    }
    uint32_t agent_handle = strtoul(agent + 1, NULL, 16);
    uint32_t client_handle = strtoul(client + 1, NULL, 16);
    _forward_agent(gate, id, agent_handle, client_handle);
    return;
  }
  if (memcmp(command, "broker", i) == 0) {
    _parm(tmp, sz, i);
    gate->broker = SkynetQueryname(ctx, command);
    return;
  }
  if (memcmp(command, "start", i) == 0) {
    _parm(tmp, sz, i);
    int uid = strtol(command, NULL, 10);
    int id = hashid_lookup(&gate->hash, uid);
    if (id >= 0) {
      SocketStart(ctx, uid);
    }
    return;
  }
  if (memcmp(command, "close", i) == 0) {
    if (gate->listen_id >= 0) {
      SocketClose(ctx, gate->listen_id);
      gate->listen_id = -1;
    }
    return;
  }
  SkynetError(ctx->GetHandle(), "[Gate] Unkown command : " + std::string(command));
}

static void _report(struct Gate *gate, const char *data, ...) {
  if (gate->watchdog == 0) {
    return;
  }
  struct Context *ctx = gate->ctx;
  va_list ap;
  va_start(ap, data);
  char tmp[1024];
  int n = vsnprintf(tmp, sizeof(tmp), data, ap);
  va_end(ap);
  ServiceSend(ctx, 0, gate->watchdog, kPtypeText, 0, tmp, n);
}

static void _forward(struct Gate *gate, struct Connection *c, int size) {
  struct Context *ctx = gate->ctx;
  int fd = c->id;
  if (fd <= 0) {
    // socket error
    return;
  }
  if (gate->broker) {
    char temp[size];
    databuffer_read(&c->buffer, &gate->mp, temp, size);
    ServiceSend(ctx, 0, gate->broker, gate->client_tag | kPtypeTagDontcopy, fd, temp, size);
    return;
  }
  if (c->agent) {
    char temp[size];
    databuffer_read(&c->buffer, &gate->mp, (char *)temp, size);
    ServiceSend(ctx, c->client, c->agent, gate->client_tag | kPtypeTagDontcopy, fd, temp, size);
  } else if (gate->watchdog) {
    char temp[size];
    int n = snprintf(temp, 32, "%d data ", c->id);
    databuffer_read(&c->buffer, &gate->mp, temp + n, size);
    ServiceSend(ctx, 0, gate->watchdog, kPtypeText | kPtypeTagDontcopy, fd, temp, size + n);
  }
}

static void dispatch_message(struct Gate *gate, struct Connection *c, int id, void *data, int sz) {
  databuffer_push(&c->buffer, &gate->mp, data, sz);
  for (;;) {
    int size = databuffer_readheader(&c->buffer, &gate->mp, gate->header_size);
    if (size < 0) {
      return;
    } else if (size > 0) {
      if (size >= 0x1000000) {
        struct Context *ctx = gate->ctx;
        databuffer_clear(&c->buffer, &gate->mp);
        SocketClose(ctx, id);
        SkynetError(ctx->GetHandle(), "Recv socket message > 16M");
        return;
      } else {
        _forward(gate, c, size);
        databuffer_reset(&c->buffer);
      }
    }
  }
}

static void dispatch_socket_message(struct Gate *gate, const struct SkynetSocketMessage *message, int sz) {
  struct Context *ctx = gate->ctx;
  switch (message->type) {
    case kSocketTypeData: {
      int id = hashid_lookup(&gate->hash, message->id);
      if (id >= 0) {
        struct Connection *c = &gate->conn[id];
        dispatch_message(gate, c, message->id, (void *)message->buffer.data(), message->ud);
      } else {
        SkynetError(ctx->GetHandle(), "Drop unknown Connection " + std::to_string(message->id) + "message");
        SocketClose(ctx, message->id);
      }
      break;
    }
    case kSocketTypeConnect: {
      if (message->id == gate->listen_id) {
        // start listening
        break;
      }
      int id = hashid_lookup(&gate->hash, message->id);
      if (id < 0) {
        SkynetError(ctx->GetHandle(), "Close unknown Connection " + std::to_string(message->id));
        SocketClose(ctx, message->id);
      }
      break;
    }
    case kSocketTypeClose:
    case kSocketTypeError: {
      int id = hashid_remove(&gate->hash, message->id);
      if (id >= 0) {
        struct Connection *c = &gate->conn[id];
        databuffer_clear(&c->buffer, &gate->mp);
        memset(c, 0, sizeof(*c));
        c->id = -1;
        _report(gate, "%d close", message->id);
      }
      break;
    }
    case kSocketTypeAccept:
      // report accept, then it will be get a SKYNET_SOCKET_TYPE_CONNECT message
      assert(gate->listen_id == message->id);
      if (hashid_full(&gate->hash)) {
        SocketClose(ctx, message->ud);
      } else {
        struct Connection *c = &gate->conn[hashid_insert(&gate->hash, message->ud)];
        if (sz >= sizeof(c->remote_name)) {
          sz = sizeof(c->remote_name) - 1;
        }
        c->id = message->ud;
        memcpy(c->remote_name, message + 1, sz);
        c->remote_name[sz] = '\0';
        _report(gate, "%d open %d %s:0", c->id, c->id, c->remote_name);
        SkynetError(ctx->GetHandle(), "socket open: " + std::to_string(c->id));
      }
      break;
    case kSocketTypeWarning:
      SkynetError(ctx->GetHandle(),
                  "fd " + std::to_string(message->id) + "send buffer " + std::to_string(message->ud) + "K");
      break;
  }
}

static int gate_cb(struct Context *ctx, void *ud, int type, int session, uint32_t source, const void *msg, size_t sz) {
  struct Gate *gate = (struct Gate *)ud;
  switch (type) {
    case kPtypeText:
      _ctrl(gate, msg, (int)sz);
      break;
    case kPtypeClient: {
      if (sz <= 4) {
        SkynetError(ctx->GetHandle(), "Invalid client message from " + std::to_string(source));
        break;
      }
      // The last 4 bytes in msg are the id of socket, write following bytes to it
      // const uint8_t *idbuf = msg + sz - 4;
      // uint32_t uid = idbuf[0] | idbuf[1] << 8 | idbuf[2] << 16 | idbuf[3] << 24;
      // int id = hashid_lookup(&gate->hash, uid);
      // if (id >= 0) {
      //   // don't send id (last 4 bytes)
      //   skynet_socket_send(ctx, uid, msg, sz - 4);
      //   // return 1 means don't free msg
      //   return 1;
      // } else {
      //   SkynetError(ctx->GetHandle(),
      //               "Invalid client id " + std::to_string((int)uid) + " from " + std::to_string(source));
      //   break;
      // }
    }
    case kPtypeSocket:
      // recv socket message from skynet_socket
      dispatch_socket_message(gate, (struct SkynetSocketMessage *)msg, (int)(sz - sizeof(struct SkynetSocketMessage)));
      break;
  }
  return 0;
}

static int start_listen(struct Gate *gate, char *listen_addr) {
  struct Context *ctx = gate->ctx;
  char *portstr = strrchr(listen_addr, ':');
  const char *host = "";
  int port;
  if (portstr == NULL) {
    port = strtol(listen_addr, NULL, 10);
    if (port <= 0) {
      SkynetError(ctx->GetHandle(), "Invalid Gate address " + std::string(listen_addr));
      return 1;
    }
  } else {
    port = strtol(portstr + 1, NULL, 10);
    if (port <= 0) {
      SkynetError(ctx->GetHandle(), "Invalid Gate address " + std::string(listen_addr));
      return 1;
    }
    portstr[0] = '\0';
    host = listen_addr;
  }
  gate->listen_id = SocketListen(ctx, host, port, kBackLog);
  if (gate->listen_id < 0) {
    return 1;
  }
  SocketStart(ctx, gate->listen_id);
  return 0;
}

extern "C" int gate_init(struct Gate *gate, struct Context *ctx, char *parm) {
  if (parm == NULL) return 1;
  int max = 0;
  int sz = strlen(parm) + 1;
  char watchdog[sz];
  char binding[sz];
  int client_tag = 0;
  char header;
  int n = sscanf(parm, "%c %s %s %d %d", &header, watchdog, binding, &client_tag, &max);
  if (n < 4) {
    SkynetError(ctx->GetHandle(), "Invalid Gate parm " + std::string(parm));
    return 1;
  }
  if (max <= 0) {
    SkynetError(ctx->GetHandle(), "Need max Connection");
    return 1;
  }
  if (header != 'S' && header != 'L') {
    SkynetError(ctx->GetHandle(), "Invalid data header style");
    return 1;
  }

  if (client_tag == 0) {
    client_tag = kPtypeClient;
  }
  if (watchdog[0] == '!') {
    gate->watchdog = 0;
  } else {
    gate->watchdog = SkynetQueryname(ctx, watchdog);
    if (gate->watchdog == 0) {
      SkynetError(ctx->GetHandle(), "Invalid watchdog " + std::string(watchdog));
      return 1;
    }
  }

  gate->ctx = ctx;

  hashid_init(&gate->hash, max);
  gate->conn = new Connection();
  gate->max_connection = max;
  int i;
  for (i = 0; i < max; i++) {
    gate->conn[i].id = -1;
  }

  gate->client_tag = client_tag;
  gate->header_size = header == 'S' ? 2 : 4;

  ServiceCallback(ctx, gate, gate_cb);

  return start_listen(gate, binding);
}
