/*
主要负责管理所有服务（ctx）的仓库（handle_storage）,存储所有ctx.

`handle`模块的主要作用包括：
- 初始化 handle_storage：Skynet 启动时，会初始化 handle_storage 结构体，该结构体包含一个读写锁、harbor（每个 Skynet
节点可配置一个唯一的 harbor，用于与其他节点组网）、handle_index（下次从 handle_index
位置开始查找空的位置）、slot_size（已分配的 ctx 的容量）、slot（slot_size 容量的数组，每一项指向一个
ctx）、name_cap（已分配 ctx 名字的容量）、name_count（已用的名字）和 name（name_cap 容量的数组，每一项是一个
handle_name）。
- 管理 ctx：当新建一个 ctx 时，会给 ctx 注册一个对应的唯一的 handle。当 kill 一个 ctx 时会回收 handle，置空 ctx 在 slot
里位置，供下一个 ctx 使用。
- 提供查询功能：Skynet 里每个ctx 都有一个唯一的 handle 对应，向某个ctx 发送消息时，都是通过 handle 找到对应的
ctx，然后向 ctx 的消息队列里 push 消息。

总的来说，`handle`模块在 Skynet
中起到了关键的作用，它负责管理和维护所有服务（ctx）的仓库（handle_storage），并提供了一系列操作来管理和查询这些服务。
*/

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "rwlock.h"
#include "service.h"

// reserve high 8 bits for remote id
constexpr int kHandleMask = 0xffffff;
constexpr int kHandleRemoteShift = 24;

constexpr int kDefaultSoltSize = 4;
constexpr int kMaxSlotSize = 0x40000000;

struct HandleName {
  std::string name;
  uint32_t handle;
};

/*
为了效率，S->lock采用读写锁，因为获取一个ctx（读）的频率要远远大于创建/杀掉（写）一个ctx的频率，所以工作线程可以并发获取ctx。
S->harbor在skynet启动文件中配置，每个skynet节点可配置唯一的harbor，用于与其他skynet节点组网，harbor占8位，所以最高可组网2^8个。
S->slot存放所有ctx指针，是一个数组，容量为S->slot_size。当new一个ctx时，会给ctx注册一个对应的唯一的handle（skynet_handle_register）。handle从S->handle_index开始循环，通过handle&(S->slot_size-1)映射成一个hash值，如果在slot中未使用，则找到ctx对应的handle，在slot存放ctx指针，并设置ctx->handle=hande。当找不到空位置（数组满）时，重新申请之前2倍容量大小的内存空间，然后把数据迁移到新的空间里。这不就是c++里vector的实现方式嘛，skynet里有很多类似的实现机制，用数组+容量实现类似vector的功能。当kill一个ctx时会回收handle（skynet_handle_retire），置空ctx在slot里位置，供下一个ctx使用。skynet里每个ctx都有一个唯一的handle对应，向某个ctx发送消息时，都是通过handle找到对应的ctx，然后向ctx的消息队列里push消息。注：handle是uint32_t，其中高8位为harbor
id，低24位为handle id，所以总共可创建2^24个ctx。
在上层逻辑很难记住每个handle具体代表哪个服务，通常会为handle注册name（不限一个），通过name找到对应的handle，通过S->name实现。S->name是一个数组，类似S->slot，动态分配内存，S->name_cap表示数组容量。S->name是按handle_name->name升序排序的，通过二分查找快速地查找name对应的handle（skynet_handle_findname）。给handle注册name时，也需保证注册完S->name有序（skynet_handle_namehandle）
*/
struct HandleStorage {
  struct rwlock lock;

  uint32_t harbor;              //每个skynet节点可配置一个唯一harbor，用于与其他节点组网
  uint32_t handle_index;        //下次从handle_index位置开始查找空的位置
  std::vector<Context *> slot;  // slot_size容量的数组，每一项指向一个ctx 大小不可修改

  int name_count;                //已用的名字
  std::vector<HandleName> name;  // name_cap容量的数组，每一项是一个handle_name 大小不可修改
};

uint32_t HandleRegister(Context &ctx);
int HandleRetire(uint32_t handle);
Context *HandleGrab(uint32_t handle);
void HandleRetireall();

uint32_t HandleFindname(std::string_view name);
std::string_view HandleNamehandle(uint32_t handle, std::string_view name);

void HandleInit(int harbor);
