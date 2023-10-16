#include "monitor.h"
#include <error.h>

#include <cstdlib>
#include <string>

#include "service.h"
#include "skynet.h"

void CMonitor::Trigger(uint32_t source, uint32_t destination) {
  this->source_ = source;
  this->destination_ = destination;
  std::atomic_fetch_add(&this->version_, decltype((&this->version_)->load())(1));
}

void CMonitor::Check() {
  if (this->version_ == this->check_version_) {
    if (this->destination_) {
      Endless(this->destination_);
      SkynetError(-1, "A message from [ :" + std::to_string(this->source_) +
                               " ] to [ :" + std::to_string(this->destination_) +
                               " ] maybe in an endless loop (version = " + std::to_string(this->version_));
    }
  } else {
    this->check_version_ = this->version_;
  }
}
