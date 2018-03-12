#ifndef PS_RDMA_H_
#define PS_RDMA_H_

#include <iostream>
#include <fcntl.h>
#include <poll.h>
#include <infiniband/verbs.h>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <queue>
#include "ps/internal/threadsafe_queue.h"

namespace ps{

#define RDMA_ERR(x) \
std::cerr << "Error, "#x << " is false" <<std::endl
#define RDMA_ERR_NE(x, y) RDMA_ERR((x) != (y))
#define RDMA_ERR_NOTNULL(x) \
std::cerr << "Error, pointer"#x << "is NULL" <<std::endl

struct config_t
{
    char        *dev_name;              // IB device name
    int         ib_port;                // local IB port to work with
    int         cq_size_max;            // cq max size
    int         sr_max;                 // send request max times in one time
    int         rr_max;                 // recv request max times in one time
    int         send_sge_max;
    int         recv_sge_max;
    size_t      sendbuf_size;           // send buffer bytes size
    size_t      recvbuf_size;           // recv buffer bytes size
};

struct rdma_dest_t
{
    uint64_t        recvbuf;            // Buffer Address
    uint32_t        rkey;               // Remote Key
    uint32_t        qp_num;             // QP Number
    uint16_t        lid;                // LID of the IB port
}__attribute__((packed));

struct qp_context_t
{
  struct ibv_qp       *qp;
  void                *recvbuf;
  void                *sendbuf;
  struct ibv_mr       *sendmr;
  struct ibv_mr       *recvmr;
  struct rdma_dest_t  rem_dest;
  struct rdma_dest_t  my_dest;
  int                 rem_offset;       //the offset for remote recv buffer
  int                 local_offset;     //the offset for local recv buffer
  std::mutex          send_mu;
  unsigned long       post_send_n;
};

struct recv_node
{
  char    *addr;
  size_t  recv_bytes;
  int     sender;
};

class RDMA
{
public:
  RDMA();
  ~RDMA();
  int                         CreateQP(int id);
  const rdma_dest_t&          GetLocalDest(int id);
  //set the remote rdma dest and modify qp into rts
  void                        SetRemoteDest(int id, rdma_dest_t dest);
  //message: size of char array(type=int) + char array
  //@para: id: qp id; size: number of chars to send(include all the bytes for size)
  //if_head: whether the offset of buffer is 0, if_signaled: whether poll the send wr
  char*                       InitData(int id, size_t size, bool*if_head, bool*if_signaled);
  int                         AddData(int id, char* buf, int size);
  int                         SendData(int id, uint64_t send_addr, int length, uint32_t imm_data);
  void                        RecvData(recv_node *node);
  void                        Receiving();

  inline void SetMyId(int id) {my_id_ = id;}
  inline void SetBufSize(int send_size, int recv_size){
    config_.sendbuf_size = send_size;
    config_.recvbuf_size = recv_size;
}
  inline bool ExistQP(int id) {return !(qps_.find(id) == qps_.end());}
  inline size_t QPsSize(){return qps_size_;}
  inline bool IsReady(){return qps_ready_num_ == qps_size_;}
  
private:
  int InitGlobal();
  int ModifyQPToInit(int id);
  int ModifyQPToRTR(int id);
  int ModifyQPToRTS(int id);
  int PostRecv(int id);
  int PostSend(int id, uint64_t send_addr, int length, uint32_t imm_data);
  /* Request notification before any completion can be created*/
  int GetEvent();

  std::unordered_map<int, qp_context_t*>  qps_;
  static const int            MAX_META_SIZE = 48;
  static const int            SEND_CQ_CYCLE=10;
  struct config_t             config_= {NULL,1,20,10,10,10,10,0,0};
  struct ibv_device           **dev_list_;
  struct ibv_device_attr      *device_attr_;
  struct ibv_port_attr        *port_attr_;
  struct ibv_context          *ib_ctx_;
  struct ibv_comp_channel     *channel_;
  struct ibv_pd               *pd_;
  struct ibv_cq               *sendcq_;
  struct ibv_cq               *recvcq_;
  int                         num_devices_;
  //The number of qps
  std::atomic<size_t>         qps_size_{0};
  //The number of qps that receive the remote destination
  std::atomic<size_t>         qps_ready_num_{0};
  ThreadsafeQueue<recv_node>  recv_node_queue_;
  int                         my_id_ = 0;
};
}
#endif