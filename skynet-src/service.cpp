#include "service.h"

#include <string.h>
#include <cassert>
#include <cstdbool>
#include <stdexcept>
#include <string>
#include <thread>

#include "env.h"
#include "error.h"
#include "handle.h"
#include "harbor.h"
#include "imp.h"
#include "log.h"
#include "monitor.h"
#include "mq.h"
#include "skynet.h"
#include "spinlock.h"
#include "timer.h"

struct Node {
  std::atomic_int total;
  int init;
  uint32_t monitor_exit;
  pthread_key_t handle_key;
  bool profile;  // default is on 配置文件
};

static struct Node G_NODE;

static void _FilterArgs(Context *ctx, int type, int *session, void **data, size_t *sz) {
  int needcopy = !(type & kPtypeTagDontcopy);
  int allocsession = type & kPtypeTagAllocsession;
  type &= 0xff;

  if (allocsession) {
    assert(*session == 0);
    *session = ctx->Newsession();
  }

  if (needcopy && *data) {
    char *msg = (char *)malloc(*sz + 1);
    memcpy(msg, *data, *sz);
    msg[*sz] = '\0';
    *data = msg;
  }

  *sz |= (size_t)type << kMessageTypeShift;
}

/*同一进程内的一个服务通过类似RPC之类的调用同一进程内的另外一个服务，并接收处理结果
param: ctx: 发送方服务实例
param: source: 发送方handle
param: destination: 接收方handle
param: type: 协议
param: session: 识别本次调用的口令，发送方发送一个消息后，保留该session，以便收到回应数据包时，能识别出是哪一次调用
*/
int ServiceSend(Context *ctx, uint32_t source, uint32_t destination, int type, int session, void *data, size_t sz) {
  if ((sz & kMessageTypeMask) != sz) {
    SkynetError(-1, "The message to " + std::to_string(destination) + " is too large");
    if (type & kPtypeTagDontcopy) {
      // skynet_free(data);
    }
    return -2;
  }
  _FilterArgs(ctx, type, &session, (void **)&data, &sz);
  if (source == 0) {
    source = ctx->handle_;
  }
  if (destination == 0) {
    if (data) {
      SkynetError(-1, "Destination address can't be 0");
      // skynet_free(data);
      return -1;
    }
    return session;
  }
  if (HarborMessageIsRemote(destination)) {
    struct RemoteMessage *rmsg = nullptr;
    rmsg->destination.handle = destination;
    rmsg->message = data;
    rmsg->sz = sz & kMessageTypeMask;
    rmsg->type = sz >> kMessageTypeShift;
    HarborSend(rmsg, source, session);
  } else {
    struct Message smsg;
    smsg.source = source;
    smsg.session = session;
    smsg.data = data;
    smsg.sz = sz;
    if (ContextPush(destination, &smsg)) {
      // skynet_free(data);
      return -1;
    }
  }
  return session;
}

static void _CopyName(char name[kGlobalNameLength], const char *addr) {
  int i;
  for (i = 0; i < kGlobalNameLength && addr[i]; i++) {
    name[i] = addr[i];
  }
  for (; i < kGlobalNameLength; i++) {
    name[i] = '\0';
  }
}

int ServiceSendname(Context *ctx, uint32_t source, const char *addr, int type, int session, void *data, size_t sz) {
  if (source == 0) {
    source = ctx->handle_;
  }
  uint32_t des = 0;
  if (addr[0] == ':') {
    des = strtoul(addr + 1, NULL, 16);
  } else if (addr[0] == '.') {
    des = HandleFindname(addr + 1);
    if (des == 0) {
      if (type & kPtypeTagDontcopy) {
        // skynet_free(data);
      }
      return -1;
    }
  } else {
    if ((sz & kMessageTypeMask) != sz) {
      // skynet_error(context, "The message to %s is too large", addr);
      if (type & kPtypeTagDontcopy) {
        // skynet_free(data);
      }
      return -2;
    }
    _FilterArgs(ctx, type, &session, (void **)&data, &sz);

    struct RemoteMessage *rmsg;
    _CopyName(rmsg->destination.name, addr);
    rmsg->destination.handle = 0;
    rmsg->message = data;
    rmsg->sz = sz & kMessageTypeMask;
    rmsg->type = sz >> kMessageTypeShift;

    HarborSend(rmsg, source, session);
    return session;
  }

  return ServiceSend(ctx, source, des, type, session, data, sz);
}

static void DropMessage(struct Message *msg, void *ud) {
  struct drop_t *d = (struct drop_t *)ud;
  // skynet_free(msg->data);
  uint32_t source = d->handle;
  assert(source);
  // report error to the message source
  ServiceSend(nullptr, source, msg->source, kPtypeError, 0, nullptr, 0);
}

Context::Context(std::string_view param, Module *mod, void *inst) {
  this->mod_ = mod;
  this->instance_ = inst;
  std::atomic_init(&this->ref_, 2);
  this->cb_ = nullptr;
  this->cb_ud_ = nullptr;
  this->session_id_ = 0;
  std::atomic_init(&this->logfile_, (FILE *)nullptr);

  this->init_ = false;
  this->endless_ = false;

  this->cpu_cost_ = 0;
  this->cpu_start_ = 0;
  this->message_count_ = 0;
  this->profile_ = G_NODE.profile;
  // Should set to 0 first to avoid handle_retireall get an uninitialized handle
  this->handle_ = 0;

  this->handle_ = HandleRegister(*this);
  MessageQueue *queue = this->queue_ = new MessageQueue(this->handle_);
  // init function maybe use this->handle, so it must init at last
  atomic_fetch_add(&G_NODE.total, decltype((&G_NODE.total)->load())(1));
}

/*增加 context 结构体实例的引用计数*/
void Context::Grab() { atomic_fetch_add(&this->ref_, decltype((&this->ref_)->load())(1)); }

/*减少 context 结构体实例的引用计数， 这里并不会实际释放掉，而是等到引用记数为0*/
bool Context::Release() {
  if (std::atomic_fetch_sub(&this->ref_, decltype((&this->ref_)->load())(1)) == 1) {
    FILE *file = std::atomic_load(&this->logfile_);
    if (file) {
      fclose(file);
    }
    ModuleRelease(*this->mod_, this->instance_);
    this->queue_->MarkRelease();
    delete this;
    std::atomic_fetch_sub(&G_NODE.total, decltype((&G_NODE.total)->load())(1));
    return false;
  }
  return true;
}

/*服务实例预留一个引用*/
void Context::Reserve() {
  this->Grab();
  // don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context
  // is 0 . the reserved context will be release at last.
  std::atomic_fetch_sub(&G_NODE.total, decltype((&G_NODE.total)->load())(1));
}

uint32_t Context::GetHandle() { return this->handle_; }

void Context::Send(void *msg, size_t sz, uint32_t source, int type, int session) {
  struct Message smsg;
  smsg.source = source;
  smsg.session = session;
  smsg.data = msg;
  smsg.sz = sz | (size_t)type << kMessageTypeShift;
  this->queue_->Push(&smsg);
}

int Context::Newsession() {
  // session always be a positive number
  int session = ++this->session_id_;
  if (session <= 0) {
    this->session_id_ = 1;
    return 1;
  }
  return session;
}

void HandleExit(Context &ctx, uint32_t handle) {
  if (handle == 0) {
    handle = ctx.handle_;
    SkynetError(ctx.GetHandle(), "KILL self");
  } else {
    SkynetError(ctx.GetHandle(), "KILL :" + std::to_string(handle));
  }
  if (G_NODE.monitor_exit) {
    ServiceSend(&ctx, handle, G_NODE.monitor_exit, kPtypeClient, 0, nullptr, 0);
  }
  HandleRetire(handle);
}

void GlobalInit() {
  std::atomic_init(&G_NODE.total, 0);
  G_NODE.monitor_exit = 0;
  G_NODE.init = 1;
  if (pthread_key_create(&G_NODE.handle_key, NULL)) {
    fprintf(stderr, "pthread_key_create failed");
    exit(1);
  }
  // set mainthread's key
  // skynet_initthread(THREAD_MAIN); TODO
}

void SkynetExit() { pthread_key_delete(G_NODE.handle_key); }

void skynet_initthread(int m) {
  uintptr_t v = (uint32_t)(-m);
  pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void ProfileEnable(int enable) { G_NODE.profile = (bool)enable; }

/*
创建一个新的服务实例，并将其添加到Skynet的服务列表中。
param: mod: 模块名
param: parm: 参数
*/
Context *NewContext(const std::string &name, const std::string &param) {
  Module *mod = Modules::Query(name);
  if (mod == nullptr) {
    throw std::runtime_error("Module " + name + " not found");
  }
  void *inst = ModuleCreate(*mod);
  if (inst == nullptr) {
    throw std::runtime_error("Failed to create instance of module " + name);
  }
  Context *ctx = new Context(param, mod, inst);

  ctx->handle_ = HandleRegister(*ctx);
  MessageQueue *queue = ctx->queue_ = new MessageQueue(ctx->handle_);
  // init function maybe use this->handle, so it must init at last
  atomic_fetch_add(&G_NODE.total, decltype((&G_NODE.total)->load())(1));
  if (ModuleInit(*mod, inst, ctx, param) == 0) {
    bool ret = ctx->Release();
    if (ret) {
      delete ctx;
    } else {
      ctx->init_ = true;
    }
    GlobalQueue::Push(queue);
    if (ret) {
      SkynetError(ctx->GetHandle(), "LAUNCH " + name + param);
    }
    return ctx;
  } else {
    SkynetError(ctx->GetHandle(), "FAILED launch " + name);
    uint32_t handle = ctx->handle_;
    ctx->Release();
    HandleRetire(handle);
    struct drop_t d = {handle};
    queue->Release((message_drop)DropMessage, &d);
    return nullptr;
  }
}

bool ContextPush(uint32_t handle, struct Message *msg) {
  Context *ctx = HandleGrab(handle);
  if (ctx == nullptr) {
    return false;
  }
  ctx->queue_->Push(msg);
  ctx->Release();
  delete ctx;
  return true;
}

int ContextTotal() { return std::atomic_load(&G_NODE.total); }

static void DispatchMessage(Context *ctx, struct Message *msg) {
  assert(ctx->init_);
  pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle_));
  int type = msg->sz >> kMessageTypeShift;
  size_t sz = msg->sz & kMessageTypeMask;
  FILE *f = (FILE *)std::atomic_load(&ctx->logfile_);
  if (f) {
    LogOutput(f, msg->source, type, msg->session, msg->data, sz);
  }
  ++ctx->message_count_;
  int reserve_msg;
  if (ctx->profile_) {
    ctx->cpu_start_ = 0;  // skynet_thread_time(); TODO
    reserve_msg = ctx->cb_(ctx, ctx->cb_ud_, type, msg->session, msg->source, msg->data, sz);
    uint64_t cost_time = 0;  // skynet_thread_time() - ctx->cpu_start_; TODO
    ctx->cpu_cost_ += cost_time;
  } else {
    reserve_msg = ctx->cb_(ctx, ctx->cb_ud_, type, msg->session, msg->source, msg->data, sz);
  }
  if (!reserve_msg) {
    msg->data = nullptr;
    // skynet_free(msg->data);
  }
}

struct MessageQueue *MessageDispatch(CMonitor *sm, struct MessageQueue *message_queue, int weight) {
  if (message_queue == nullptr) {
    message_queue = GlobalQueue::Pop();
    if (message_queue == nullptr) return nullptr;
  }

  uint32_t handle = message_queue->GetHandle();
  ;
  Context *ctx = HandleGrab(handle);
  if (ctx == nullptr) {
    struct drop_t d = {handle};
    message_queue->Release((message_drop)DropMessage, &d);
    return GlobalQueue::Pop();
  }

  int n = 1;
  struct Message msg;
  for (int i = 0; i < n; i++) {
    if (message_queue->Pop()) {
      ctx->Release();
      return GlobalQueue::Pop();
    } else if (i == 0 && weight >= 0) {
      n = message_queue->Length();
      n >>= weight;
    }
    int overload = message_queue->Overload();
    if (overload) {
      // skynet_error(ctx, "May overload, message queue length = %d", overload);
    }

    // todo
    // skynet_monitor_trigger(sm, msg.source, handle);

    if (ctx->cb_ == nullptr) {
      msg.data = nullptr;
      // skynet_free(msg.data);
    } else {
      DispatchMessage(ctx, &msg);
    }

    // TODO
    // skynet_monitor_trigger(sm, 0, 0);
  }

  assert(message_queue == ctx->queue_);
  struct MessageQueue *message_queue_temp = GlobalQueue::Pop();
  if (message_queue_temp) {
    // If global mq is not empty , push q back, and return next queue (nq)
    // Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
    GlobalQueue::Push(message_queue);
    message_queue = message_queue_temp;
  }
  ctx->Release();
  return message_queue;
}

void Context::DispatchAll() {
  // for skynet_error
  struct Message msg;
  struct MessageQueue *message_queue = this->queue_;
  while (!message_queue->Pop()) {
    DispatchMessage(this, &msg);
  }
}

void Endless(uint32_t handle) {
  Context *ctx = HandleGrab(handle);
  if (ctx == nullptr) {
    return;
  }
  ctx->endless_ = true;
  ctx->Release();
}

uint32_t GetMoniterExit() { return G_NODE.monitor_exit; }

void SetMonitorExit(uint32_t monitor_exit) { G_NODE.monitor_exit = monitor_exit; }

int ServiceIsRemote(Context *ctx, uint32_t handle, int *harbor) {
  int ret = 0;  // skynet_harbor_message_isremote(handle);
  if (harbor) {
    *harbor = (int)(handle >> kHandleRemoteShift);
  }
  return ret;
}

void ServiceCallback(Context *ctx, void *ud, skynet_cb cb) {
  ctx->cb_ = cb;
  ctx->cb_ud_ = ud;
}

uint32_t ServiceCurrentHandle() {
  if (G_NODE.init) {
    void *handle = pthread_getspecific(G_NODE.handle_key);
    return (uint32_t)(uintptr_t)handle;
  } else {
    uint32_t v = (uint32_t)(-THREAD_MAIN);
    return v;
  }
}