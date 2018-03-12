#include "ps/ps.h"
#include <sys/time.h>
using namespace ps;

void StartServer() {
  if (!IsServer()) return;
  auto server = new KVServer<float>(0);
  server->set_request_handle(KVServerDefaultHandle<float>());
  RegisterExitCallback([server](){ delete server; });
}

void RunWorker() {
  if (!IsWorker()) return;
  KVWorker<float> kv(0);

  // init
  int num = 1000000;
  std::vector<Key> keys(num);
  std::vector<float> vals(num);

  int rank = MyRank();
  srand(rank + 7);
  for (int i = 0; i < num; ++i) {
    keys[i] = kMaxKey / num * i + rank;
    vals[i] = (rand() % 1000);
  }

  // push
  int repeat = 1000;
  //c: timestamp
  std::vector<int> ts;
  for (int i = 0; i < repeat; ++i) {
    ts.push_back(kv.Push(keys, vals));

    // to avoid too frequency push, which leads huge memory usage
    if (i > 10) kv.Wait(ts[ts.size()-10]);
  }
  for (int t : ts) kv.Wait(t);

  // pull
  std::vector<float> rets;
  kv.Wait(kv.Pull(keys, &rets));

  float res = 0;
  for (int i = 0; i < num; ++i) {
    res += fabs(rets[i] - vals[i] * repeat);
  }
  CHECK_LT(res / repeat, 1e-5);
  LL << "error: " << res / repeat;
}

int main(int argc, char *argv[]) {
  // setup server nodes
  StartServer();
  // start system
  Start();
  // run worker nodes
  struct timeval tp;
  gettimeofday(&tp, NULL);
  unsigned long start_msec, end_msec;
  start_msec = tp.tv_sec * 1000 + tp.tv_usec / 1000;
  RunWorker();
  // stop system
  Finalize();
  
  gettimeofday(&tp, NULL);
  end_msec = tp.tv_sec * 1000 + tp.tv_usec / 1000;
  fprintf(stdout, "Finish communication, millseconds:%lu\n", end_msec - start_msec);
  return 0;
}
