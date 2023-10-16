#include "timer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

#include "error.h"
#include "handle.h"
#include "mq.h"
#include "service.h"
#include "skynet.h"
#include "spinlock.h"

static Timer *kTimer = nullptr;

static inline struct TimerNode *LinkClear(struct LinkList *link_list) {
  struct TimerNode *ret = link_list->head.next;
  link_list->head.next = nullptr;
  link_list->tail = &(link_list->head);
  return ret;
}

static inline void Link(struct LinkList *link_list, struct TimerNode *timer_node) {
  link_list->tail->next = timer_node;
  link_list->tail = timer_node;
  timer_node->next = nullptr;
}

void Timer::AddNode(struct TimerNode *timer_node) {
  spinlock_lock(&this->lock);
  uint32_t expire_time = timer_node->expire;
  uint32_t current_time = this->time_;

  if ((expire_time | TIME_NEAR_MASK) == (current_time | TIME_NEAR_MASK)) {
    Link(&this->near_[expire_time & TIME_NEAR_MASK], timer_node);
  } else {
    int i;
    uint32_t mask = TIME_NEAR << TIME_LEVEL_SHIFT;
    for (i = 0; i < 3; i++) {
      if ((expire_time | (mask - 1)) == (current_time | (mask - 1))) {
        break;
      }
      mask <<= TIME_LEVEL_SHIFT;
    }

    Link(&this->t_[i][((expire_time >> (TIME_NEAR_SHIFT + i * TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)], timer_node);
  }
  spinlock_unlock(&this->lock);
}

void Timer::MoveList(int level, int idx) {
  struct TimerNode *timer_node = LinkClear(&this->t_[level][idx]);
  while (timer_node) {
    struct TimerNode *temp = timer_node->next;
    this->AddNode(timer_node);
    timer_node = temp;
  }
}

void Timer::TimerShift() {
  int mask = TIME_NEAR;
  uint32_t ct = ++this->time_;
  if (ct == 0) {
    this->MoveList(3, 0);
  } else {
    uint32_t time = ct >> TIME_NEAR_SHIFT;
    int i = 0;

    while ((ct & (mask - 1)) == 0) {
      int idx = time & TIME_LEVEL_MASK;
      if (idx != 0) {
        this->MoveList(i, idx);
        break;
      }
      mask <<= TIME_LEVEL_SHIFT;
      time >>= TIME_LEVEL_SHIFT;
      ++i;
    }
  }
}

static inline void DispatchList(struct TimerNode *current) {
  do {
    struct Message message;
    message.source = 0;
    message.session = current->session;
    message.data = NULL;
    message.sz = (size_t)kPtypeResponse << kMessageTypeShift;

    ContextPush(current->handle, &message);
    struct TimerNode *temp = current;
    current = current->next;
    delete temp;
  } while (current);
}

void Timer::Execute() {
  int idx = this->time_ & TIME_NEAR_MASK;

  while (this->near_[idx].head.next) {
    struct TimerNode *current = LinkClear(&this->near_[idx]);
    spinlock_unlock(&this->lock);
    // DispatchList don't need lock T
    DispatchList(current);
    spinlock_lock(&this->lock);
  }
}

void Timer::Update() {
  spinlock_lock(&this->lock);

  // try to dispatch timeout 0 (rare condition)
  this->Execute();

  // shift time first, and then dispatch Timer message
  this->TimerShift();

  this->Execute();

  spinlock_unlock(&this->lock);
}

int Timeout(uint32_t handle, int time, int session) {
  if (time <= 0) {
    struct Message message;
    message.source = 0;
    message.session = session;
    message.data = NULL;
    message.sz = (size_t)kPtypeResponse << kMessageTypeShift;

    if (ContextPush(handle, &message)) {
      return -1;
    }
  } else {
    struct TimerNode timer_node;
    timer_node.handle = handle;
    timer_node.session = session;
    timer_node.expire = time + kTimer->time_;
    kTimer->AddNode(&timer_node);
  }

  return session;
}

// centisecond: 1/100 second
static void systime(uint32_t *sec, uint32_t *cs) {
  struct timespec ti;
  clock_gettime(CLOCK_REALTIME, &ti);
  *sec = (uint32_t)ti.tv_sec;
  *cs = (uint32_t)(ti.tv_nsec / 10000000);
}

static uint64_t GetTime() {
  uint64_t t;
  struct timespec ti;
  clock_gettime(CLOCK_MONOTONIC, &ti);
  t = (uint64_t)ti.tv_sec * 100;
  t += ti.tv_nsec / 10000000;
  return t;
}

void Updatetime() {
  uint64_t cp = GetTime();
  if (cp < kTimer->current_point_) {
    SkynetError(-1,
                "time diff error: change from " + std::to_string(cp)  + " to " + std::to_string(kTimer->current_point_));
    kTimer->current_point_ = cp;
  } else if (cp != kTimer->current_point_) {
    uint32_t diff = (uint32_t)(cp - kTimer->current_point_);
    kTimer->current_point_ = cp;
    kTimer->current_ += diff;
    int i;
    for (i = 0; i < diff; i++) {
      kTimer->Update();
    }
  }
}

uint32_t Starttime() { return kTimer->starttime_; }

uint64_t Now() { return kTimer->current_; }

// for profile
#define NANOSEC 1000000000
#define MICROSEC 1000000

uint64_t ThreadTime() {
  struct timespec timespec;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &timespec);
  return (uint64_t)timespec.tv_sec * MICROSEC + (uint64_t)timespec.tv_nsec / (NANOSEC / MICROSEC);
}

void TimerInit(void) {
  kTimer = new Timer();
  for (int i = 0; i < TIME_NEAR; i++) {
    LinkClear(&kTimer->near_[i]);
  }

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < TIME_LEVEL; j++) {
      LinkClear(&kTimer->t_[i][j]);
    }
  }

  spinlock_init(&kTimer->lock);

  kTimer->current_ = 0;
  uint32_t current = 0;

  struct timespec ti;
  clock_gettime(CLOCK_REALTIME, &ti);
  kTimer->starttime_ = (uint32_t)ti.tv_sec;
  kTimer->current_ = (uint32_t)(ti.tv_nsec / 10000000);
  kTimer->current_ = current;
  kTimer->current_point_ = GetTime();
}

void TimerExit() { delete kTimer; }
