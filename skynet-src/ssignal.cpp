#include "ssignal.h"

int Sigign() {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPIPE, &sa, 0);
  return 0;
}