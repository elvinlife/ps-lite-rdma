### Introduction for ps-lite

A light and efficient implementation of the parameter server
framework. It provides clean yet powerful APIs. For example, a worker node can
communicate with the server nodes by

- `Push(keys, values)`: push a list of (key, value) pairs to the server nodes
- `Pull(keys)`: pull the values from servers for a list of keys
- `Wait`: wait untill a push or pull finished.

A simple example:

```c++
  std::vector<uint64_t> key = {1, 3, 5};
  std::vector<float> val = {1, 1, 1};
  std::vector<float> recv_val;
  ps::KVWorker<float> w;
  w.Wait(w.Push(key, val));
  w.Wait(w.Pull(key, &recv_val));
```

More features:

- Flexible and high-performance communication: zero-copy push/pull, supporting
  dynamic length values, user-defined filters for communication compression
- Server-side programming: supporting user-defined handles on server nodes
- Add rdma tech for communication using `<infiniband/verbs.h>`


### Build

`ps-lite-rdma` requires a C++11 compiler such as `g++ >= 4.8`. 

Then clone and build

```bash
git clone https://github.com/elvinlife/ps-lite-rdma.git
cd ps-lite-rdma && make -j
```

### Test

You can test the ps-lite-rdma performance using the shell script provided in test-rdma.

Before testing, you should set `NUM`, `BUGINFO`, `TESTFILE`

* test local performance:

```shell
make local
```

* test multiple machines performance
  * for scheduler

  ```shell
  make scheduler
  ```

  * for server

  ```shell
  make server
  ```

  * for worker

  ```shell
  make worker
  ```

### Others

The other part of readme is the same as that in original ps-lite.
