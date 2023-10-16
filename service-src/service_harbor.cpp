/*
        harbor listen the kPtypeHarbor (in text)
        N name : update the global name
        S fd id: connect to new harbor , we should send self_id to fd first , and then recv a id (check it), and at last
   send queue. A fd id: accept new harbor , we should send self_id to fd , and then send queue.

        If the fd is disconnected, send message to slave in kPtypeText.  D id
        If we don't known a globalname, send message to slave in kPtypeText. Q name
 */

#include <assert.h>
#include <error.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "handle.h"
#include "harbor.h"
#include "service.h"
#include "skynet.h"
#include "socket.h"
#include "socket_protocol.h"
#include "socket_server.h"

constexpr int kHashSize = 4098;
constexpr int kDefaultQueueSize = 1024;

// 12 is sizeof(struct remote_message_header)
constexpr int kHeaderCookieLength = 12;

constexpr int kStatusWait = 0;
constexpr int kStatusHandshake = 1;
constexpr int kStatusHeader = 2;
constexpr int kStatusContent = 3;
constexpr int kStatusDown = 4;

/*
        message type (8bits) is in destination high 8bits
        harbor id (8bits) is also in that place , but remote message doesn't need harbor id.
 */
struct remote_message_header {
  uint32_t source;
  uint32_t destination;
  uint32_t session;
};

struct harbor_msg {
  struct remote_message_header header;
  void *buffer;
  size_t size;
};

struct harbor_msg_queue {
  int size;
  int head;
  int tail;
  std::vector<harbor_msg> data;
};

struct keyvalue {
  struct keyvalue *next;
  char key[kGlobalNameLength];
  uint32_t hash;
  uint32_t value;
  struct harbor_msg_queue *queue;
};

struct hashmap {
  struct keyvalue *node[kHashSize];
};

struct slave {
  int fd;
  struct harbor_msg_queue *queue;
  int status;
  int length;
  int read;
  uint8_t size[4];
  std::string recv_buffer;
};

struct harbor {
  struct Context *ctx;
  int id;
  uint32_t slave;
  struct hashmap *map;
  struct slave s[kRemoteMax];
};

// hash table

static void push_queue_msg(struct harbor_msg_queue *queue, struct harbor_msg *m) {
  // If there is only 1 free slot which is reserved to distinguish full/empty
  // of circular buffer, expand it.
  if (((queue->tail + 1) % queue->size) == queue->head) {
    std::vector<harbor_msg> new_buffer;
    new_buffer.resize(queue->size * 2);
    int i;
    for (i = 0; i < queue->size - 1; i++) {
      new_buffer[i] = queue->data[(i + queue->head) % queue->size];
    }
    queue->data = new_buffer;
    queue->head = 0;
    queue->tail = queue->size - 1;
    queue->size *= 2;
  }
  struct harbor_msg *slot = &queue->data[queue->tail];
  *slot = *m;
  queue->tail = (queue->tail + 1) % queue->size;
}

static void push_queue(struct harbor_msg_queue *queue, void *buffer, size_t sz, struct remote_message_header *header) {
  struct harbor_msg m;
  m.header = *header;
  m.buffer = buffer;
  m.size = sz;
  push_queue_msg(queue, &m);
}

static struct harbor_msg *pop_queue(struct harbor_msg_queue *queue) {
  if (queue->head == queue->tail) {
    return NULL;
  }
  struct harbor_msg *slot = &queue->data[queue->head];
  queue->head = (queue->head + 1) % queue->size;
  return slot;
}

static struct harbor_msg_queue *new_queue() {
  struct harbor_msg_queue *queue = new harbor_msg_queue();
  queue->size = kDefaultQueueSize;
  queue->head = 0;
  queue->tail = 0;
  queue->data.resize(kDefaultQueueSize);

  return queue;
}

static void release_queue(struct harbor_msg_queue *queue) {
  if (queue == NULL) return;
  struct harbor_msg *m;
  while ((m = pop_queue(queue)) != NULL) {
    // delete m->buffer;
  }
  delete queue;
}

static struct keyvalue *hash_search(struct hashmap *hash, const char name[kGlobalNameLength]) {
  uint32_t *ptr = (uint32_t *)name;
  uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
  struct keyvalue *node = hash->node[h % kHashSize];
  while (node) {
    if (node->hash == h && strncmp(node->key, name, kGlobalNameLength) == 0) {
      return node;
    }
    node = node->next;
  }
  return NULL;
}

/*

// Don't support erase name yet

static struct void
hash_erase(struct hashmap * hash, char name[kGlobalNameLength) {
        uint32_t *ptr = name;
        uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
        struct keyvalue ** ptr = &hash->node[h % kHashSize];
        while (*ptr) {
                struct keyvalue * node = *ptr;
                if (node->hash == h && strncmp(node->key, name, kGlobalNameLength) == 0) {
                        _release_queue(node->queue);
                        *ptr->next = node->next;
                        SkynetError(node);
                        return;
                }
                *ptr = &(node->next);
        }
}
*/

static struct keyvalue *hash_insert(struct hashmap *hash, const char name[kGlobalNameLength]) {
  uint32_t *ptr = (uint32_t *)name;
  uint32_t h = ptr[0] ^ ptr[1] ^ ptr[2] ^ ptr[3];
  struct keyvalue **pkv = &hash->node[h % kHashSize];
  struct keyvalue *node = new keyvalue();
  memcpy(node->key, name, kGlobalNameLength);
  node->next = *pkv;
  node->queue = NULL;
  node->hash = h;
  node->value = 0;
  *pkv = node;

  return node;
}

static struct hashmap *hash_new() {
  struct hashmap *h = new hashmap();
  memset(h, 0, sizeof(*h));
  return h;
}

static void hash_delete(struct hashmap *hash) {
  int i;
  for (i = 0; i < kHashSize; i++) {
    struct keyvalue *node = hash->node[i];
    while (node) {
      struct keyvalue *next = node->next;
      release_queue(node->queue);
      delete node;
      node = next;
    }
  }
  delete hash;
}

///////////////

static void close_harbor(struct harbor *h, int id) {
  struct slave *s = &h->s[id];
  s->status = kStatusDown;
  if (s->fd) {
    SocketClose(h->ctx, s->fd);
    s->fd = 0;
  }
  if (s->queue) {
    release_queue(s->queue);
    s->queue = NULL;
  }
}

static void report_harbor_down(struct harbor *h, int id) {
  char down[64];
  int n = sprintf(down, "D %d", id);
  ServiceSend(h->ctx, 0, h->slave, kPtypeText, 0, down, n);
}

struct harbor *harbor_create(void) {
  struct harbor *h = new harbor();
  memset(h, 0, sizeof(*h));
  h->map = hash_new();
  return h;
}

static void close_all_remotes(struct harbor *h) {
  int i;
  for (i = 1; i < kRemoteMax; i++) {
    close_harbor(h, i);
    // don't call report_harbor_down.
    // never call ServiceSend during module exit, because of dead lock
  }
}

void harbor_release(struct harbor *h) {
  close_all_remotes(h);
  hash_delete(h->map);
  delete h;
}

static inline void to_bigendian(uint8_t *buffer, uint32_t n) {
  buffer[0] = (n >> 24) & 0xff;
  buffer[1] = (n >> 16) & 0xff;
  buffer[2] = (n >> 8) & 0xff;
  buffer[3] = n & 0xff;
}

static inline void header_to_message(const struct remote_message_header *header, uint8_t *message) {
  to_bigendian(message, header->source);
  to_bigendian(message + 4, header->destination);
  to_bigendian(message + 8, header->session);
}

static inline uint32_t from_bigendian(uint32_t n) {
  union {
    uint32_t big;
    uint8_t bytes[4];
  } u;
  u.big = n;
  return u.bytes[0] << 24 | u.bytes[1] << 16 | u.bytes[2] << 8 | u.bytes[3];
}

static inline void message_to_header(const uint32_t *message, struct remote_message_header *header) {
  header->source = from_bigendian(message[0]);
  header->destination = from_bigendian(message[1]);
  header->session = from_bigendian(message[2]);
}

// socket package

static void forward_local_messsage(struct harbor *h, void *msg, int sz) {
  const char *cookie = (const char *)msg;
  cookie += sz - kHeaderCookieLength;
  struct remote_message_header header;
  message_to_header((const uint32_t *)cookie, &header);

  uint32_t destination = header.destination;
  int type = destination >> kHandleRemoteShift;
  destination = (destination & kHandleMask) | ((uint32_t)h->id << kHandleRemoteShift);

  if (ServiceSend(h->ctx, header.source, destination, type | kPtypeTagDontcopy, (int)header.session, (void *)msg,
                  sz - kHeaderCookieLength) < 0) {
    if (type != kPtypeError) {
      // don't need report error when type is error
      ServiceSend(h->ctx, destination, header.source, kPtypeError, (int)header.session, NULL, 0);
    }
    SkynetError(h->ctx->GetHandle(), "Unknown destination :" + std::to_string(destination) + " from :" +
                                         std::to_string(header.source) + " type(" + std::to_string(type) + ")");
  }
}

static void send_remote(struct Context *ctx, int fd, const char *buffer, size_t sz,
                        struct remote_message_header *cookie) {
  size_t sz_header = sz + sizeof(*cookie);
  if (sz_header > UINT32_MAX) {
    SkynetError(ctx->GetHandle(), "remote message from :" + std::to_string(cookie->source) +
                                      " to :" + std::to_string(cookie->destination) + " is too large.");
    return;
  }
  uint8_t sendbuf[sz_header + 4];
  to_bigendian(sendbuf, (uint32_t)sz_header);
  memcpy(sendbuf + 4, buffer, sz);
  header_to_message(cookie, sendbuf + 4 + sz);

  struct Sendbuffer tmp;
  tmp.id = fd;
  tmp.type = SOCKET_BUFFER_RAWPOINTER;
	std::string str((char*)sendbuf);
  tmp.buffer = &str;
  tmp.sz = sz_header + 4;

  // ignore send error, because if the connection is broken, the mainloop will recv a message.
  SocketSendBuffer(tmp);
}

static void dispatch_name_queue(struct harbor *h, struct keyvalue *node) {
  struct harbor_msg_queue *queue = node->queue;
  uint32_t handle = node->value;
  int harbor_id = handle >> kHandleRemoteShift;
  struct Context *context = h->ctx;
  struct slave *s = &h->s[harbor_id];
  int fd = s->fd;
  if (fd == 0) {
    if (s->status == kStatusDown) {
      char tmp[kGlobalNameLength + 1];
      memcpy(tmp, node->key, kGlobalNameLength);
      tmp[kGlobalNameLength] = '\0';
      std::string str = tmp;
      SkynetError(context->GetHandle(), "Drop message to " + str + " (in harbor " + std::to_string(harbor_id) + ")");
    } else {
      if (s->queue == NULL) {
        s->queue = node->queue;
        node->queue = NULL;
      } else {
        struct harbor_msg *m;
        while ((m = pop_queue(queue)) != NULL) {
          push_queue_msg(s->queue, m);
        }
      }
      if (harbor_id == (h->slave >> kHandleRemoteShift)) {
        // the harbor_id is local
        struct harbor_msg *m;
        while ((m = pop_queue(s->queue)) != NULL) {
          int type = m->header.destination >> kHandleRemoteShift;
          ServiceSend(context, m->header.source, handle, type | kPtypeTagDontcopy, m->header.session, m->buffer,
                      m->size);
        }
        release_queue(s->queue);
        s->queue = NULL;
      }
    }
    return;
  }
  struct harbor_msg *m;
  while ((m = pop_queue(queue)) != NULL) {
    m->header.destination |= (handle & kHandleMask);
    send_remote(context, fd, (char *)m->buffer, m->size, &m->header);
  }
}

static void dispatch_queue(struct harbor *h, int id) {
  struct slave *s = &h->s[id];
  int fd = s->fd;
  assert(fd != 0);

  struct harbor_msg_queue *queue = s->queue;
  if (queue == NULL) return;

  struct harbor_msg *m;
  while ((m = pop_queue(queue)) != NULL) {
    send_remote(h->ctx, fd, (char *)m->buffer, m->size, &m->header);
  }
  release_queue(queue);
  s->queue = NULL;
}

static void push_socket_data(struct harbor *h, const struct SkynetSocketMessage *message) {
  assert(message->type == kSocketTypeData);
  int fd = message->id;
  int i;
  int id = 0;
  struct slave *s = NULL;
  for (i = 1; i < kRemoteMax; i++) {
    if (h->s[i].fd == fd) {
      s = &h->s[i];
      id = i;
      break;
    }
  }
  if (s == NULL) {
    SkynetError(h->ctx->GetHandle(), "Invalid socket fd (" + std::to_string(fd) + ") data");
    return;
  }
  uint8_t *buffer = (uint8_t *)message->buffer.data();
  int size = message->ud;

  for (;;) {
    switch (s->status) {
      case kStatusHandshake: {
        // check id
        uint8_t remote_id = buffer[0];
        if (remote_id != id) {
          SkynetError(h->ctx->GetHandle(), "Invalid shakehand id (" + std::to_string(id) + ") from fd = " +
                                               std::to_string(fd) + " , harbor = " + std::to_string(remote_id));
          close_harbor(h, id);
          return;
        }
        ++buffer;
        --size;
        s->status = kStatusHeader;

        dispatch_queue(h, id);

        if (size == 0) {
          break;
        }
        // go though
      }
      case kStatusHeader: {
        // big endian 4 bytes length, the first one must be 0.
        int need = 4 - s->read;
        if (size < need) {
          memcpy(s->size + s->read, buffer, size);
          s->read += size;
          return;
        } else {
          memcpy(s->size + s->read, buffer, need);
          buffer += need;
          size -= need;

          if (s->size[0] != 0) {
            SkynetError(h->ctx->GetHandle(), "Message is too long from harbor " + std::to_string(id));
            close_harbor(h, id);
            return;
          }
          s->length = s->size[1] << 16 | s->size[2] << 8 | s->size[3];
          s->read = 0;
          s->status = kStatusContent;
          if (size == 0) {
            return;
          }
        }
      }
      // go though
      case kStatusContent: {
        int need = s->length - s->read;
        if (size < need) {
					s->recv_buffer += std::string((char*)buffer);
          s->read += size;
          return;
        }
				s->recv_buffer += std::string((char*)buffer);
        //memcpy(s->recv_buffer + s->read, buffer, need);
        forward_local_messsage(h, s->recv_buffer.data(), s->length);
        s->length = 0;
        s->read = 0;
        s->recv_buffer = "";
        size -= need;
        buffer += need;
        s->status = kStatusHeader;
        if (size == 0) return;
        break;
      }
      default:
        return;
    }
  }
}

static void update_name(struct harbor *h, const char name[kGlobalNameLength], uint32_t handle) {
  struct keyvalue *node = hash_search(h->map, name);
  if (node == NULL) {
    node = hash_insert(h->map, name);
  }
  node->value = handle;
  if (node->queue) {
    dispatch_name_queue(h, node);
    release_queue(node->queue);
    node->queue = NULL;
  }
}

static int remote_send_handle(struct harbor *h, uint32_t source, uint32_t destination, int type, int session,
                              const char *msg, size_t sz) {
  int harbor_id = destination >> kHandleRemoteShift;
  struct Context *context = h->ctx;
  if (harbor_id == h->id) {
    // local message
    ServiceSend(context, source, destination, type | kPtypeTagDontcopy, session, (void *)msg, sz);
    return 1;
  }

  struct slave *s = &h->s[harbor_id];
  if (s->fd == 0 || s->status == kStatusHandshake) {
    if (s->status == kStatusDown) {
      // throw an error return to source
      // report the destination is dead
      ServiceSend(context, destination, source, kPtypeError, session, NULL, 0);
      // SkynetError(context->GetHandle(), "Drop message to harbor %d from %x to %x (session = %d, msgsz = %d)", harbor_id, source,
      //             destination, session, (int)sz);
    } else {
      if (s->queue == NULL) {
        s->queue = new_queue();
      }
      struct remote_message_header header;
      header.source = source;
      header.destination = (type << kHandleRemoteShift) | (destination & kHandleMask);
      header.session = (uint32_t)session;
      push_queue(s->queue, (void *)msg, sz, &header);
      return 1;
    }
  } else {
    struct remote_message_header cookie;
    cookie.source = source;
    cookie.destination = (destination & kHandleMask) | ((uint32_t)type << kHandleRemoteShift);
    cookie.session = (uint32_t)session;
    send_remote(context, s->fd, msg, sz, &cookie);
  }

  return 0;
}

static int remote_send_name(struct harbor *h, uint32_t source, const char name[kGlobalNameLength], int type,
                            int session, const char *msg, size_t sz) {
  struct keyvalue *node = hash_search(h->map, name);
  if (node == NULL) {
    node = hash_insert(h->map, name);
  }
  if (node->value == 0) {
    if (node->queue == NULL) {
      node->queue = new_queue();
    }
    struct remote_message_header header;
    header.source = source;
    header.destination = type << kHandleRemoteShift;
    header.session = (uint32_t)session;
    push_queue(node->queue, (void *)msg, sz, &header);
    char query[2 + kGlobalNameLength + 1] = "Q ";
    query[2 + kGlobalNameLength] = 0;
    memcpy(query + 2, name, kGlobalNameLength);
    ServiceSend(h->ctx, 0, h->slave, kPtypeText, 0, query, strlen(query));
    return 1;
  } else {
    return remote_send_handle(h, source, node->value, type, session, msg, sz);
  }
}

static void handshake(struct harbor *h, int id) {
  struct slave *s = &h->s[id];
  uint8_t handshake[1] = {(uint8_t)h->id};
  struct Sendbuffer tmp;
  tmp.id = s->fd;
  tmp.type = SOCKET_BUFFER_RAWPOINTER;
	std::string str((char*)handshake);
  tmp.buffer = &str;
  tmp.sz = 1;
	SocketSendBuffer(tmp);
}

static void harbor_command(struct harbor *h, const char *msg, size_t sz, int session, uint32_t source) {
  const char *name = msg + 2;
  int s = (int)sz;
  s -= 2;
  switch (msg[0]) {
    case 'N': {
      if (s <= 0 || s >= kGlobalNameLength) {
        // SkynetError(h->ctx->GetHandle(), "Invalid global name %s", name);
        return;
      }
      struct RemoteName rn;
      memset(&rn, 0, sizeof(rn));
      memcpy(rn.name, name, s);
      rn.handle = source;
      update_name(h, rn.name, rn.handle);
      break;
    }
    case 'S':
    case 'A': {
      char buffer[s + 1];
      memcpy(buffer, name, s);
      buffer[s] = 0;
      int fd = 0, id = 0;
      sscanf(buffer, "%d %d", &fd, &id);
      if (fd == 0 || id <= 0 || id >= kRemoteMax) {
        // SkynetError(h->ctx, "Invalid command %c %s", msg[0], buffer);
        return;
      }
      struct slave *slave = &h->s[id];
      if (slave->fd != 0) {
        // SkynetError(h->ctx, "Harbor %d alreay exist", id);
        return;
      }
      slave->fd = fd;

			SocketStart(h->ctx, fd);
      handshake(h, id);
      if (msg[0] == 'S') {
        slave->status = kStatusHandshake;
      } else {
        slave->status = kStatusHeader;
        dispatch_queue(h, id);
      }
      break;
    }
    default:
      //SkynetError(h->ctx, "Unknown command %s", msg);
      return;
  }
}

static int harbor_id(struct harbor *h, int fd) {
  int i;
  for (i = 1; i < kRemoteMax; i++) {
    struct slave *s = &h->s[i];
    if (s->fd == fd) {
      return i;
    }
  }
  return 0;
}

static int mainloop(struct Context *context, void *ud, int type, int session, uint32_t source, const void *msg,
                    size_t sz) {
  struct harbor *h = (struct harbor *)ud;
  switch (type) {
    case kPtypeSocket: {
      const struct SkynetSocketMessage *message = (const struct SkynetSocketMessage *)msg;
      switch (message->type) {
        case kSocketTypeData:
          push_socket_data(h, message);
          // SkynetError(message->buffer);
          break;
        case kSocketTypeError:
        case kSocketTypeClose: {
          int id = harbor_id(h, message->id);
          if (id) {
            report_harbor_down(h, id);
          } else {
            SkynetError(context->GetHandle(), "Unkown fd ("+std::to_string(message->id)+") closed");
          }
          break;
        }
        case kSocketTypeConnect:
          // fd forward to this service
          break;
        case kSocketWarning: {
          int id = harbor_id(h, message->id);
          if (id) {
            //SkynetError(context, "message havn't send to Harbor (%d) reach %d K", id, message->ud);
          }
          break;
        }
        default:
          // SkynetError(context, "recv invalid socket message type %d", type);
          break;
      }
      return 0;
    }
    case kPtypeHarbor: {
      harbor_command(h, (const char *)msg, sz, session, source);
      return 0;
    }
    case kPtypeSystem: {
      // remote message out
      const struct RemoteMessage *rmsg = (const struct RemoteMessage *)msg;
      if (rmsg->destination.handle == 0) {
        if (remote_send_name(h, source, rmsg->destination.name, rmsg->type, session, (const char *)rmsg->message, rmsg->sz)) {
          return 0;
        }
      } else {
        if (remote_send_handle(h, source, rmsg->destination.handle, rmsg->type, session, (const char *)rmsg->message, rmsg->sz)) {
          return 0;
        }
      }
      // SkynetError((void *)rmsg->message);
      return 0;
    }
    default:
      // SkynetError(context, "recv invalid message from %x,  type = %d", source, type);
      if (session != 0 && type != kPtypeError) {
        ServiceSend(context, 0, source, kPtypeError, session, NULL, 0);
      }
      return 0;
  }
}

int harbor_init(struct harbor *h, struct Context *ctx, const char *args) {
  h->ctx = ctx;
  int harbor_id = 0;
  uint32_t slave = 0;
  sscanf(args, "%d %u", &harbor_id, &slave);
  if (slave == 0) {
    return 1;
  }
  h->id = harbor_id;
  h->slave = slave;
  if (harbor_id == 0) {
    close_all_remotes(h);
  }
  ServiceCallback(ctx, h, mainloop);
  HarborStart(ctx);

  return 0;
}
