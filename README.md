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
- Support for 


### Build

`ps-lite-rdma` requires a C++11 compiler such as `g++ >= 4.8`. 

Then clone and build

```bash
git clone https://github.com/elvinlife/ps-lite-rdma.git
cd ps-lite-rdma && make -j4
```

### Test

You can test the ps-lite-rdma performance using the shell script provided in `./tests-rdma`.

* for local machine

```
./s_tests-rdma/rdma_local.sh [# of servers] [# of workers] [bin to test] [args...]
```

* for multiple machines

  same as local machine, just change the script file according to the role of the current machine

### About

This is the core source code for USTC team in The 5th Student RDMA Programming Competition. Our team won the first prize in the end.


​				
​			