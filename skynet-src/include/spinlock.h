#pragma once

#include <immintrin.h>  // For _mm_pause
#include <atomic>

struct Spinlock {
  std::atomic_int lock;
};

static inline void spinlock_init(struct Spinlock *lock) { std::atomic_init(&lock->lock, 0); }

static inline void spinlock_lock(struct Spinlock *lock) {
  while(true) {
    if (!std::atomic_exchange_explicit(&lock->lock, 1, std::memory_order_acquire)) return;
    while (std::atomic_load_explicit(&lock->lock, std::memory_order_relaxed)) _mm_pause();
  }
}

static inline int spinlock_trylock(struct Spinlock *lock) {
  return !std::atomic_load_explicit(&lock->lock, std::memory_order_relaxed) &&
         !std::atomic_exchange_explicit(&lock->lock, 1, std::memory_order_acquire);
}

static inline void spinlock_unlock(struct Spinlock *lock) {
  std::atomic_store_explicit(&lock->lock, 0, std::memory_order_release);
}

static inline void spinlock_destroy(struct Spinlock *lock) { (void)lock; }
