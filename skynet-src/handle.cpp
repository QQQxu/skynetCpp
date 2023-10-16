#include "handle.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>

#include "service.h"

// #include "skynet.h"

static struct HandleStorage *kHandleStorage;

uint32_t HandleRegister(Context &ctx) {
  rwlock_wlock(&kHandleStorage->lock);

  while (true) {
    uint32_t handle = kHandleStorage->handle_index;
    for (int i = 0; i < kHandleStorage->slot.size(); i++, handle++) {
      if (handle > kHandleMask) {
        // 0 is reserved
        handle = 1;
      }
      int hash = handle & (kHandleStorage->slot.size() - 1);
      if (kHandleStorage->slot[hash] == NULL) {
        kHandleStorage->slot[hash] = &ctx;
        kHandleStorage->handle_index = handle + 1;

        rwlock_wunlock(&kHandleStorage->lock);

        handle |= kHandleStorage->harbor;
        return handle;
      }
    }

    assert((kHandleStorage->slot.size() * 2 - 1) <= kHandleMask);

    std::vector<Context *> new_slot;
    new_slot.resize(kHandleStorage->slot.size() * 2);

    for (int i = 0; i < kHandleStorage->slot.size(); ++i) {
      if (kHandleStorage->slot[i]) {
        int hash = kHandleStorage->slot[i]->GetHandle() & (kHandleStorage->slot.size() - 1);
        assert(kHandleStorage->slot[hash] == nullptr);
        new_slot[hash] = kHandleStorage->slot[i];
      }
    }
    kHandleStorage->slot = new_slot;
  }
}

int HandleRetire(uint32_t handle) {
  int ret = 0;

  rwlock_wlock(&kHandleStorage->lock);

  uint32_t hash = handle & (kHandleStorage->slot.size() - 1);
  Context *ctx = kHandleStorage->slot[hash];

  if (ctx != NULL && ctx->GetHandle() == handle) {
    kHandleStorage->slot[hash] = NULL;
    ret = 1;
    int i;
    int j = 0, n = kHandleStorage->name_count;
    for (i = 0; i < n; ++i) {
      if (kHandleStorage->name[i].handle == handle) {
        kHandleStorage->name[i].name = "";
        continue;
      } else if (i != j) {
        kHandleStorage->name[j] = kHandleStorage->name[i];
      }
      ++j;
    }
    kHandleStorage->name_count = j;
  } else {
    ctx = NULL;
  }

  rwlock_wunlock(&kHandleStorage->lock);

  if (ctx) {
    // release ctx may call skynet_handle_* , so wunlock first.
    ctx->Release();
  }

  return ret;
}

void HandleRetireall() {
  while (true) {
    int n = 0;
    for (int i = 0; i < kHandleStorage->slot.size(); ++i) {
      rwlock_rlock(&kHandleStorage->lock);
      Context *ctx = kHandleStorage->slot[i];
      uint32_t handle = 0;
      if (ctx) {
        handle = ctx->GetHandle();
        ++n;
      }
      rwlock_runlock(&kHandleStorage->lock);
      if (handle != 0) {
        HandleRetire(handle);
      }
    }
    if (n == 0) return;
  }
}

Context *HandleGrab(uint32_t handle) {
  Context *result = nullptr;

  rwlock_rlock(&kHandleStorage->lock);

  uint32_t hash = handle & (kHandleStorage->slot.size() - 1);
  Context *ctx = kHandleStorage->slot[hash];
  if (ctx && ctx->GetHandle() == handle) {
    result = ctx;
    result->Grab();
  }

  rwlock_runlock(&kHandleStorage->lock);

  return result;
}

uint32_t HandleFindname(std::string_view name) {
  rwlock_rlock(&kHandleStorage->lock);

  uint32_t handle = 0;

  int begin = 0;
  int end = kHandleStorage->name_count - 1;
  while (begin <= end) {
    int mid = (begin + end) / 2;
    struct HandleName *handle_name = &kHandleStorage->name[mid];
    if (handle_name->name == name) {
      handle = handle_name->handle;
      break;
    } else if (handle_name->name < name) {
      begin = mid + 1;
    } else {
      end = mid - 1;
    }
  }
  rwlock_runlock(&kHandleStorage->lock);

  return handle;
}

std::string_view HandleNamehandle(uint32_t handle, std::string_view name) {
  rwlock_wlock(&kHandleStorage->lock);

  int begin = 0;
  int end = kHandleStorage->name_count - 1;
  while (begin <= end) {
    int mid = (begin + end) / 2;
    struct HandleName *handle_name = &kHandleStorage->name[mid];
    if (handle_name->name == name) {
      return "";
    } else if (handle_name->name < name) {
      begin = mid + 1;
    } else {
      end = mid - 1;
    }
  }

  if (kHandleStorage->name_count >= kHandleStorage->name.size()) {
    assert(kHandleStorage->name.size() * 2 <= kMaxSlotSize);

    kHandleStorage->name.resize(kHandleStorage->name.size() * 2);
  }
  for (int i = kHandleStorage->name_count; i > begin; i--) {
    kHandleStorage->name[i] = kHandleStorage->name[i - 1];
  }
  kHandleStorage->name[begin].name = name;
  kHandleStorage->name[begin].handle = handle;
  kHandleStorage->name_count++;
  rwlock_wunlock(&kHandleStorage->lock);

  return name;
}

void HandleInit(int harbor) {
  assert(kHandleStorage == nullptr);
  struct HandleStorage *handle_storage = new HandleStorage();
  handle_storage->slot.resize(kDefaultSoltSize);

  rwlockInit(&handle_storage->lock);
  // reserve 0 for system
  handle_storage->harbor = (uint32_t)(harbor & 0xff) << kHandleRemoteShift;
  handle_storage->handle_index = 1;
  handle_storage->name_count = 0;
  handle_storage->name.resize(2);

  kHandleStorage = handle_storage;
  // Don't need to free H
}
