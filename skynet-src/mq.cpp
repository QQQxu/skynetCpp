#include "mq.h"

#include <cassert>
#include <string>
#include <vector>

#include "handle.h"
#include "skynet.h"
#include "spinlock.h"

MessageQueue::MessageQueue(uint32_t handle) {
  this->handle_ = handle;
  spinlock_init(&this->lock);
  // When the queue is create (always between service create and service init) ,
  // set in_global flag to avoid push it to global queue .
  // If the service init success, NewContext(name, parm) will call skynet_mq_push to push it to global queue.
  this->in_global_ = kMqInGlobal;
  this->release = false;
  this->overload_ = 0;
  this->overload_threshold_ = kMqOverload;
  this->next_ = nullptr;
}

MessageQueue::~MessageQueue() {
  assert(this->next_ == nullptr);
  spinlock_destroy(&this->lock);
}

/*
标记为可释放，并添加入全局队列
*/
void MessageQueue::MarkRelease() {
  spinlock_lock(&this->lock);
  assert(this->release == false);
  this->release = true;
  if (this->in_global_ != kMqInGlobal) {
    GlobalQueue::Push(this);
  }
  spinlock_unlock(&this->lock);
}

/*skynet_mq_release函数的主要作用是释放指定的消息队列，并处理其中所有剩余的消息
 */
void MessageQueue::Release(message_drop drop_func, struct drop_t *drop_param) {
  spinlock_lock(&this->lock);

  if (this->release) {
    spinlock_unlock(&this->lock);
    struct Message *msg = this->Pop();
    while (msg != nullptr) {
      drop_func(msg, drop_param);
      msg = this->Pop();
    }
    assert(this->next_ == nullptr);
    delete this;
  } else {
    GlobalQueue::Push(this);
    spinlock_unlock(&this->lock);
  }
}

uint32_t MessageQueue::GetHandle() { return this->handle_; }

struct Message *MessageQueue::Pop() {
  spinlock_lock(&this->lock);
  struct Message *result_msg;
  if (!this->message_queue_.empty()){
    result_msg = &this->message_queue_.front();
    while(this->message_queue_.size() > this->overload_threshold_){
      this->overload_ = this->message_queue_.size();
      this->overload_threshold_ *= 2;
    }
  }else {
    // reset overload_threshold when queue is empty
    this->overload_threshold_ = kMqOverload;
    this->in_global_ = 0;
  }
  spinlock_unlock(&this->lock);

  return result_msg;
}

void MessageQueue::Push(struct Message *msg) {
  assert(msg);
  spinlock_lock(&this->lock);

  this->message_queue_.push(*msg);
  if (this->in_global_ == 0) {
    this->in_global_ = kMqInGlobal;
    GlobalQueue::Push(this);
  }

  spinlock_unlock(&this->lock);
}

int MessageQueue::Length() {
  int length;

  spinlock_lock(&this->lock);
  length = this->message_queue_.size();
  spinlock_unlock(&this->lock);
  return length;
}

int MessageQueue::Overload() {
  if (this->overload_) {
    int overload = this->overload_;
    this->overload_ = 0;
    return overload;
  }
  return 0;
}

static GlobalQueue kGlobalQueue;

/*将一个消息推送到指定的消息队列中*/
void GlobalQueue::Push(MessageQueue *queue) {
  GlobalQueue *q = &kGlobalQueue;
  
  spinlock_lock(&q->lock);
  assert(queue->next_ == nullptr);
  if (q->tail_) {
    q->tail_->next_ = queue;
    q->tail_ = queue;
  } else {
    q->head_ = q->tail_ = queue;
  }
  spinlock_unlock(&q->lock);
}

MessageQueue *GlobalQueue::Pop() {
  GlobalQueue *q = &kGlobalQueue;
  spinlock_lock(&q->lock);
  MessageQueue *mq = q->head_;
  if (mq) {
    q->head_ = mq->next_;
    if (q->head_ == nullptr) {
      assert(mq == q->tail_);
      q->tail_ = nullptr;
    }
    mq->next_ = nullptr;
  }
  spinlock_unlock(&q->lock);

  return mq;
}

void MessageQueueInit() {
  spinlock_init(&kGlobalQueue.lock);
}