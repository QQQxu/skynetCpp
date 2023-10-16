#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <functional>

#include "module.h"
#include "monitor.h"
#include "mq.h"

struct skynet_message;

using skynet_cb = std::function<int(Context *ctx, void *ud, int type, int session, uint32_t source, const void *msg, size_t sz)>;


class Context {
 public:
  Context(std::string_view param, Module *mod, void *inst);
  void Grab();
  bool Release();
  void Reserve();
  uint32_t GetHandle();
  void Send(void *msg, size_t sz, uint32_t source, int type, int session);
  int Newsession();
  void DispatchAll();  // for skynet_error output before exit

 public:
  void *instance_;  // 服务实例
  Module *mod_;     // 管理动态库函数
  void *cb_ud_;     //用于回调的实例
  skynet_cb cb_;    // 回调函数
  MessageQueue *queue_;  // 消息队列列表,每个服务都有一个消息队列，当队列中有消息时，会主动挂到全局链表

  std::atomic<FILE *> logfile_;  //指向服务的日志文件
  uint32_t handle_;              // 唯一标识
  int session_id_;               // 当前处理的消息id，用来判断是否卡死在同一个消息处理中
  std::atomic_int ref_;          // 服务引用计数
  int message_count_;            // 已处理过的消息总数
  bool init_;                    // 初始化成功的标识
  bool endless_;                 //死循环标识

  bool profile_;        // cpu 性能指标开启开关
  uint64_t cpu_cost_;   // in microsec
  uint64_t cpu_start_;  // in microsec
  std::string result_;  // 存放性能指标的查询结果
};

Context *NewContext(const std::string &name, const std::string &param) ;
bool ContextPush(uint32_t handle, struct Message *message);
int ContextTotal();
struct MessageQueue *MessageDispatch(CMonitor *, struct MessageQueue *, int weight);  // return next queue
void Endless(uint32_t handle);                                                       // for monitor

void GlobalInit();
void SkynetExit();
void skynet_initthread(int m);

void ProfileEnable(int enable);

void HandleExit(Context &ctx, uint32_t handle);

uint32_t GetMoniterExit();
void SetMonitorExit(uint32_t monitor_exit);

int ServiceSend(Context *ctx, uint32_t source, uint32_t destination, int type, int session, void *data, size_t sz);
int ServiceSendname(Context *ctx, uint32_t source, const char *addr, int type, int session, void *data, size_t sz);
int ServiceIsRemote(Context *ctx, uint32_t handle, int *harbor);
void ServiceCallback(Context *ctx, void *ud, skynet_cb cb);
uint32_t ServiceCurrentHandle();