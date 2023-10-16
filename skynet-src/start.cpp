#include "start.h"

#include <unistd.h>
#include <cassert>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <error.h>
#include "handle.h"
#include "harbor.h"
#include "imp.h"
#include "module.h"
#include "monitor.h"
#include "mq.h"
#include "service.h"
#include "skynet.h"
#include "socket.h"
#include "timer.h"

struct MonitorThread {
  int count;
  std::vector<CMonitor *> monitors;
  std::condition_variable cond_var;
  std::mutex mutex;
  bool thread_unlock = false;
  int sleep;
  int quit;
};

struct ThreadWorkerParm {
  struct MonitorThread *m;
  int id;
  int weight;
};

static volatile int SIG = 0;

static void handle_hup(int signal) {
  if (signal == SIGHUP) {
    SIG = 1;
  }
}

static void CreateThread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
  if (pthread_create(thread, NULL, start_routine, arg)) {
    fprintf(stderr, "Create thread failed");
    exit(1);
  }

  std::thread t(start_routine, arg);
}

static void FreeMonitor(struct MonitorThread *m) {
  int i;
  int n = m->count;
  for (i = 0; i < n; i++) {
    delete m->monitors[i];
  }
  m->monitors.clear();
  delete m;
}

static void *ThreadMonitor(struct MonitorThread *p) {
  struct MonitorThread *m = p;
  int n = m->count;
  skynet_initthread(THREAD_MONITOR);
  while (true) {
    if (ContextTotal() == 0) break;
    for (int i = 0; i < n; i++) {
      m->monitors[i]->Check();
    }
    for (int i = 0; i < 5; i++) {
      if (ContextTotal() == 0) break;
      sleep(1);
    }
  }
  return nullptr;
}

static void *ThreadWorker(struct ThreadWorkerParm *p) {
  struct ThreadWorkerParm *wp = p;
  int id = wp->id;
  int weight = wp->weight;
  struct MonitorThread *m = wp->m;
  CMonitor *monitor = m->monitors[id];
  skynet_initthread(THREAD_WORKER);
  struct MessageQueue *message_queue = nullptr;
  while (!m->quit) {
    message_queue = MessageDispatch(monitor, message_queue, weight);
    if (message_queue == nullptr) {
      std::unique_lock<std::mutex> unique_lock(m->mutex);
      ++m->sleep;
      // "spurious wakeup" is harmless,
      // because skynet_context_message_dispatch() can be call at any time.
      if (!m->quit) {
        m->cond_var.wait(unique_lock, [m]() { return m->thread_unlock; });
      };
      --m->sleep;
    }
  }
  return nullptr;
}

static void WakeUp(struct MonitorThread *m, int busy) {
  if (m->sleep >= m->count - busy) {
    // signal sleep worker, "spurious wakeup" is harmless
    m->cond_var.notify_all();
  }
}

static void signal_hup() {
  // make log file reopen

  struct Message smsg;
  smsg.source = 0;
  smsg.session = 0;
  smsg.data = NULL;
  smsg.sz = (size_t)kPtypeSystem << kMessageTypeShift;
  uint32_t logger = HandleFindname("logger");
  if (logger) {
    ContextPush(logger, &smsg);
  }
}

static void *ThreadTimer(struct MonitorThread *p) {
  struct MonitorThread *m = p;
  skynet_initthread(THREAD_TIMER);
  while (true) {
    Updatetime();
    SocketUpdatetime();
    if (ContextTotal() == 0) break;
    WakeUp(m, m->count - 1);
    usleep(2500);
    if (SIG) {
      signal_hup();
      SIG = 0;
    }
  }
  // wakeup socket thread
  SocketExit();
  {
    // wakeup all worker thread
    std::unique_lock<std::mutex> unique_lock(m->mutex);
    m->quit = 1;
    m->cond_var.notify_all();
  }
  return nullptr;
}

static void *ThreadSocket(struct MonitorThread *p) {
  struct MonitorThread *m = p;
  skynet_initthread(THREAD_SOCKET);
  while (true) {
    int r = SocketPoll();
    if (r == 0) break;
    if (r < 0) {
      if (ContextTotal() == 0) break;
      continue;
    }
    WakeUp(m, 0);
  }
  return nullptr;
}

static void Start(int thread) {
  std::vector<std::thread> threads;  // 存储线程对象的容器

  struct MonitorThread *monitorthread = new MonitorThread();
  monitorthread->count = thread;
  monitorthread->sleep = 0;

  monitorthread->monitors.resize(thread);
  for (int i = 0; i < thread; i++) {
    monitorthread->monitors[i] = new CMonitor();
  }

  threads.emplace_back(ThreadMonitor, monitorthread);
  threads.emplace_back(ThreadTimer, monitorthread);
  threads.emplace_back(ThreadSocket, monitorthread);

  static int weight[] = {
      -1, -1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
  };
  struct ThreadWorkerParm wp[thread];
  for (int i = 0; i < thread; i++) {
    wp[i].m = monitorthread;
    wp[i].id = i;
    if (i < sizeof(weight) / sizeof(weight[0])) {
      wp[i].weight = weight[i];
    } else {
      wp[i].weight = 0;
    }
    threads.emplace_back(ThreadWorker, &wp[i]);
  }

  for (std::thread &t : threads) {
    t.join();
  }

  FreeMonitor(monitorthread);
}

/*引导服务*/
static void Bootstrap(Context *logger, const std::string &name, const std::string &args) {
  Context *ctx = NewContext(name, args);
  if (ctx == nullptr) {
    SkynetError(-1, "Bootstrap error : " + std::string(name) + std::string(args) + "\n");
    logger->DispatchAll();
    exit(1);
  }
}

void SkynetStart(struct Config *config) {
  // register SIGHUP for log file reopen
  // struct sigaction sa;
  // sa.sa_handler = &handle_hup;
  // sa.sa_flags = SA_RESTART;
  // sigfillset(&sa.sa_mask);
  // sigaction(SIGHUP, &sa, NULL);

  // if (config->daemon) {
  //   if (daemon_init(config->daemon)) {
  //     exit(1);
  //   }
  // }
  HarborInit(config->harbor);  // 节点模块
  HandleInit(1);
  MessageQueueInit();
  SkynetModuleInit(config->module_path);
  TimerInit();
  SocketInit();
  ProfileEnable(config->profile);

  Context *ctx = NewContext(config->logservice, config->logger);
  if (ctx == nullptr) {
    std::cerr << "Can't launch " << config->logservice << " service\n" << std::endl;
    exit(1);
  }

  HandleNamehandle(ctx->GetHandle(), "logger");

  // Bootstrap(ctx, config->bootstrap_name, config->bootstrap_args);

  Start(config->thread);

  // harbor_exit may call socket send, so it should exit before socket_free
  HarborExit();
  SocketFree();
  // if (config->daemon) {
  //   daemon_exit(config->daemon);
  // }
}