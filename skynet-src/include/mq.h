/*
Skynet的消息队列分为两级，一个全局消息队列，它包含一个头尾指针，分别指向两个隶属于指定服务的次级消息队列。
Skynet中的每一个服务，都有一个唯一的、专属的次级消息队列。
*/
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <queue>

#include "spinlock.h"

// type is encoding in message.sz high 8bit
constexpr size_t kMessageTypeMask = (SIZE_MAX >> 8);
constexpr size_t kMessageTypeShift = ((sizeof(size_t) - 1) * 8);

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.
constexpr int kMqInGlobal = 1;
constexpr int kMqOverload = 1024;

struct drop_t {
  uint32_t handle;
};

using message_drop = void (*)(struct Message *, void *);

/*
次级消息队列，管理每一个指定服务
这些变量共同构成了一个消息队列，用于存储待处理的消息。消息队列在 Skynet
中起到了非常重要的作用，它允许不同的服务之间进行消息的异步通信
*/
class MessageQueue {
 public:
  MessageQueue(uint32_t handle);
  ~MessageQueue();

  void MarkRelease();
  void Release(message_drop drop_func, struct drop_t *ud);
  uint32_t GetHandle();

  struct Message *Pop();
  void Push(struct Message *message);

  // return the length of message queue, for debug
  int Length();
  int Overload();

  struct Spinlock lock;  // 用于保护消息队列的自旋锁，防止多线程同时访问造成竞态条件
  uint32_t handle_;      // 消息队列所属的服务的句柄

  std::queue<struct Message> message_queue_;  // 消息队列的容量，即队列可以容纳的消息数量上限
  bool release;                              // 标识消息队列是否已释放的标记
  int in_global_;                             // 标识消息队列是否在全局消息队列列表中的标记

  int overload_;            // 标识消息队列是否超载的标记
  int overload_threshold_;  // 消息队列超载的阈值

  MessageQueue *next_;  // 指向下一个消息队列的指针，用于将消息队列连接成链表。
};

/*
全局消息队列
*/
class GlobalQueue {
 public:
  MessageQueue *head_;
  MessageQueue *tail_;
  struct Spinlock lock;

 public:
  static void Push(MessageQueue *queue);
  static MessageQueue *Pop();
};

struct Message {
 public:
  uint32_t source;
  int session;
  void *data;
  size_t sz;
};

void MessageQueueInit();
