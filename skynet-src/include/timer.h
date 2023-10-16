#pragma once

#include <cstddef>
#include <cstdint>
#include "spinlock.h"

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR - 1)
#define TIME_LEVEL_MASK (TIME_LEVEL - 1)

typedef void (*timer_execute_func)(void *ud, void *arg);

struct TimerNode {
  struct TimerNode *next;
  uint32_t expire;
  uint32_t handle;
  int session;
};

struct LinkList {
  struct TimerNode head;
  struct TimerNode *tail;
};

class Timer {
 public:
  void AddNode(struct TimerNode *timer_node);
  void Add(void *arg, size_t sz, int time);
  void Execute();
  void Update();
  void MoveList(int level, int idx);
  void TimerShift();

 public:
  struct LinkList near_[TIME_NEAR];
  struct LinkList t_[4][TIME_LEVEL];
  struct Spinlock lock;
  uint32_t time_;
  uint32_t starttime_;
  uint64_t current_;
  uint64_t current_point_;
};

int Timeout(uint32_t handle, int time, int session);
void Updatetime();
uint32_t Starttime();
uint64_t Now();
uint64_t ThreadTime();  // for profile, in micro second

void TimerInit();
void TimerExit();
