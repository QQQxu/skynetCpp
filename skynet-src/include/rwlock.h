#pragma once

#include <atomic>

struct rwlock {
  std::atomic_int write;
  std::atomic_int read;
};

static inline void rwlockInit(struct rwlock *lock) {
  std::atomic_init(&lock->write, 0);
  std::atomic_init(&lock->read, 0);
}

static inline void rwlock_rlock(struct rwlock *lock) {
  while(true) {
    while (std::atomic_load(&lock->write)) {
    }
    std::atomic_fetch_add(&lock->read, decltype((&lock->read)->load())(1));
    if (std::atomic_load(&lock->write)) {
      std::atomic_fetch_sub(&lock->read, decltype((&lock->read)->load())(1));
    } else {
      break;
    }
  }
}

static inline void rwlock_wlock(struct rwlock *lock) {
  int val = 0;
  while (!std::atomic_compare_exchange_weak(&lock->write, &(val), 1)) {
  }
  while (std::atomic_load(&lock->read)) {
  }
}

static inline void rwlock_wunlock(struct rwlock *lock) { std::atomic_store(&lock->write, 0); }

static inline void rwlock_runlock(struct rwlock *lock) {
  std::atomic_fetch_sub(&lock->read, decltype((&lock->read)->load())(1));
}
