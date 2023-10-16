#pragma once

#include <cstdint>
#include <atomic>

/*
监控模块
*/

class CMonitor {
 public:
  void Trigger(uint32_t source, uint32_t destination);
  void Check();

 public:
  std::atomic_int version_;
  int check_version_;
  uint32_t source_;
  uint32_t destination_;
};
