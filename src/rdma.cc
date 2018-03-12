#include "ps/internal/rdma.h"
#include <cstdlib>
#include <mutex>
#include <fcntl.h>
#include <sys/time.h>
#include <cstring>

namespace ps{
/*Initiate the static variabels*/
RDMA::RDMA()
  :dev_list_(NULL), 
  device_attr_(NULL), 
  port_attr_(NULL),
  ib_ctx_(NULL),
  channel_(NULL),
  pd_(NULL),
  sendcq_(NULL),
  recvcq_(NULL),
  num_devices_(0)
{
  fprintf(stderr, "Init rdma resource\n");
  if (InitGlobal())
    std::cerr << "Fail to initialize RDMA global resources" << std::endl;
  else
    std::cout << "Succeed to initialize RDMA glocal resources" << std::endl;
}

RDMA::~RDMA()
{
  for (auto it = qps_.begin(); it != qps_.end(); ++it)
  {
    int id = it->first;
    struct qp_context_t *qp_ctx = it->second;
    if(qp_ctx->qp)
      if (ibv_destroy_qp(qp_ctx->qp))
        fprintf(stderr, "Fail to destroy %d QP\n", id);
    if(qp_ctx->sendmr)
      if(ibv_dereg_mr(qp_ctx->sendmr))
        fprintf(stderr, "Fail to deregister send mr %d QP\n", id);
    if(qp_ctx->recvmr)
      if(ibv_dereg_mr(qp_ctx->recvmr))
        fprintf(stderr, "Fail to deregister recv mr %d QP\n", id);
    if(qp_ctx->recvbuf)
      free(qp_ctx->recvbuf);
    if(qp_ctx->sendbuf)
      free(qp_ctx->sendbuf);
    delete qp_ctx;
  }
  if(recvcq_)
    if (ibv_destroy_cq(recvcq_))
      fprintf(stderr, "Fail to destroy Recv CQ\n");
  if(sendcq_)
    if (ibv_destroy_cq(sendcq_))
      fprintf(stderr, "Fail to destroy Send CQ\n");
  if(pd_)
    if (ibv_dealloc_pd(pd_))
      fprintf(stderr, "Fail to deallocate PD\n");
  if(channel_)
    if (ibv_destroy_comp_channel(channel_))
      fprintf(stderr, "Fail to destroy channel\n");
  if(ib_ctx_)
    if (ibv_close_device(ib_ctx_))
      fprintf(stderr, "Fail to close device context\n");  
  if(dev_list_)
    ibv_free_device_list(dev_list_);
}

int RDMA::CreateQP(int id)
{
  qp_context_t *qp_ctx = new qp_context_t();
  // allocate memory buffer
  void *recvbuf = malloc(config_.recvbuf_size);
  void *sendbuf = malloc(config_.sendbuf_size);
  if (!recvbuf)
  {
    RDMA_ERR_NOTNULL(recvbuf);
    return 1;
  }
  if (!sendbuf)
  {
    RDMA_ERR_NOTNULL(sendbuf);
    return 1;
  }
  // register memory buffer
  int recv_mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
  int send_mr_flags = IBV_ACCESS_LOCAL_WRITE;
  struct ibv_mr *recvmr = ibv_reg_mr(pd_, recvbuf, config_.recvbuf_size, recv_mr_flags);
  struct ibv_mr *sendmr = ibv_reg_mr(pd_, sendbuf, config_.sendbuf_size, send_mr_flags);
  if (!recvmr)
  {
    RDMA_ERR_NOTNULL(recvmr);
    return 1;
  }
  if (!sendmr)
  {
    RDMA_ERR_NOTNULL(sendmr);
    return 1;
  }
  struct ibv_qp_init_attr qp_init_attr = {};
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.sq_sig_all = 0;
  qp_init_attr.send_cq = sendcq_;
  qp_init_attr.recv_cq = recvcq_;
  qp_init_attr.cap.max_send_wr = config_.sr_max;
  qp_init_attr.cap.max_recv_wr = config_.rr_max;
  qp_init_attr.cap.max_send_sge = config_.send_sge_max;
  qp_init_attr.cap.max_recv_sge = config_.recv_sge_max;
  struct ibv_qp *qp = ibv_create_qp(pd_, &qp_init_attr);
  if (!qp)
  {
    RDMA_ERR_NOTNULL(qp);
    return 1;
  }
  qp_ctx->qp = qp;
  qp_ctx->recvbuf = recvbuf;
  qp_ctx->sendbuf = sendbuf;
  qp_ctx->sendmr = sendmr;
  qp_ctx->recvmr = recvmr;
  qp_ctx->my_dest.recvbuf = (uintptr_t)recvbuf;
  qp_ctx->my_dest.rkey = recvmr->rkey;
  qp_ctx->my_dest.qp_num = qp->qp_num;
  qp_ctx->my_dest.lid = port_attr_->lid;
  qp_ctx->rem_offset = 0;
  qp_ctx->local_offset = 0;
  qp_ctx->post_send_n = 0;
  qps_[id] = qp_ctx;
  PostRecv(id);
  qps_size_++;
  return 0;
}

const rdma_dest_t& RDMA::GetLocalDest(int id)
{
  return qps_[id]->my_dest;
}

void RDMA::SetRemoteDest(int id, rdma_dest_t dest)
{
  qps_[id]->rem_dest = dest;
  bool is_ready = true;
  if (ModifyQPToInit(id)) is_ready = false;
  if (ModifyQPToRTR(id)) is_ready = false;
  if (ModifyQPToRTS(id)) is_ready = false;

  //ReqNotifyCQ(recvcq_, 0);
  if (is_ready)
    qps_ready_num_ ++;
  return;
}

char* RDMA::InitData(int id, size_t size, bool* if_head, bool* if_signaled)
{
  qp_context_t *qp_ctx = qps_[id];
  size += MAX_META_SIZE;
  char* addr = (char*)qps_[id]->sendbuf;
  (qp_ctx->send_mu).lock();
  if (qp_ctx->rem_offset + size > config_.recvbuf_size)
    qp_ctx->rem_offset = 0;
  addr += qp_ctx->rem_offset;

  if (qp_ctx->rem_offset == 0)
    *if_head = true;

  qp_ctx->post_send_n += 1;
  if (qp_ctx->post_send_n % SEND_CQ_CYCLE == 0)
    *if_signaled = true;

  qp_ctx->rem_offset += size;
  (qp_ctx->send_mu).unlock();

  return addr;
}

int RDMA::SendData(int id, uint64_t send_addr, int length, uint32_t imm_data)
{
  bool if_signaled = imm_data & 1<<18;           //if imm_data's 18th bit is 1
  int ret = PostSend(id, send_addr, length, imm_data);
  if (ret)
  {
    fprintf(stderr, "node%d: fail to send data to node%d\n", my_id_, id);
    return 1;
  }
  if (if_signaled)
  {
    struct ibv_wc wc;
    while(true)
    {
      ret = ibv_poll_cq(sendcq_, 1, &wc);
      if (ret != 0)
        break;
    }
    if (ret < 0){
      fprintf(stderr, "node%d: fail to poll send wr completion sent to node%d\n", my_id_, id);
      return 1;  
    }
  }
  return 0;
}

int RDMA::GetEvent(){
    /* 配置句柄使其非阻塞 */
  struct pollfd my_pollfd;
  int ms_timeout = 1;         // edit
  my_pollfd.fd      = channel_->fd;
  my_pollfd.events  = POLLIN;
  my_pollfd.revents = 0;
  int rc;
  do {
    rc = poll(&my_pollfd, 1, ms_timeout);
  }while (rc == 0);
  if (rc < 0) {
    fprintf(stderr, "node%d poll failed\n", my_id_);
    return 1;
  }
  struct ibv_cq *ev_cq;
  void *ev_ctx = NULL;
  ev_cq = recvcq_;
  if(ibv_get_cq_event(channel_, &ev_cq, &ev_ctx)){
    fprintf(stderr, "node%d fail to get cq event\n", my_id_);
    return 1;
  }
  ibv_ack_cq_events(ev_cq, 1);
  return 0;
}

void RDMA::RecvData(recv_node* node)
{
  recv_node_queue_.WaitAndPop(node);
}

void RDMA::Receiving()
{
  struct ibv_wc wc;
  int rc;
  uint32_t imm_data, recv_bytes;
  recv_node node = {};
  while(true)
  {
    rc = ibv_poll_cq(recvcq_, 1, &wc);
    if (rc == 0)
    {
      GetEvent();
      if (ibv_req_notify_cq(recvcq_, 0))
      {
        fprintf(stderr, "Couldn't request CQ notification\n");
        continue;
      }
      int nwc = ibv_poll_cq(recvcq_, 1, &wc);
      if(nwc == 0)
        continue;
      else
      {
        imm_data = wc.imm_data;
        recv_bytes = wc.byte_len;
      }
    }
    else if (rc > 0)
    {
      if (wc.status != IBV_WC_SUCCESS)
      {
        fprintf(stderr, "Error Node ID %d: RDMA Recv wc status wrong.\n", my_id_);
        continue;
      }
      else
      {
        imm_data = wc.imm_data;
        recv_bytes = wc.byte_len;
      }
    }
    else
    {
      fprintf(stderr, "Error Node ID %d: RDMA Recv Poll recvcq failed. Exit Code: %d\n",my_id_, rc);
      continue;
    }
    int send_id = (imm_data & 0x0000ffff);
    PostRecv(send_id);
    if (!ExistQP(send_id))
      fprintf(stderr, "node%d: RDMA recv thread: error, didn't find sender qp%d\n", my_id_, send_id);
    qp_context_t *qp_ctx = qps_[send_id];
    if (imm_data & (1<<17))
      qp_ctx->local_offset = 0;
    node.addr = (char*)qp_ctx->recvbuf + qp_ctx->local_offset;
    node.sender = send_id;
    node.recv_bytes = recv_bytes;
    recv_node_queue_.Push(node);

    int meta_byte = *((int*)node.addr) + sizeof(int);
    qp_ctx->local_offset += ((int)recv_bytes - meta_byte + MAX_META_SIZE);
    if (imm_data & (1<<16))
      break;
  }
}

int RDMA::InitGlobal()
{
    // Get the devices list
    dev_list_ = ibv_get_device_list(&num_devices_);
    if (!dev_list_)
    {
      RDMA_ERR_NOTNULL(dev_list_);
      return 1;
    }
    if (num_devices_ == 0)
    {
      RDMA_ERR_NE(num_devices_, 0);
      return 1;
    }
    // Get the ibv device
    struct ibv_device   *ibv_dev = NULL;
    if (!config_.dev_name)
        ibv_dev = *dev_list_;
    else
    {
        for (int i=0; i < num_devices_; i++)
        {
            if (!strcmp(config_.dev_name, ibv_get_device_name(dev_list_[i])))
            {
                ibv_dev = dev_list_[i];
                break;
            }
        }
    }
    if (!ibv_dev)
    {
      RDMA_ERR_NOTNULL(ibv_dev);
      return 1;
    }
    // Get the ibv_device's context
    ib_ctx_ = ibv_open_device(ibv_dev);
    if (!ib_ctx_)
    {
      RDMA_ERR_NOTNULL(ib_ctx_);
      return 1;
    }
    device_attr_ = (ibv_device_attr*)malloc(sizeof(ibv_device_attr));
    if (ibv_query_device(ib_ctx_, device_attr_)) 
    {
        fprintf(stderr, "Error, failed to query the device '%s' attributes\n",
            ibv_get_device_name(ibv_dev));
        return 1;
    }
    port_attr_ = (ibv_port_attr*)malloc(sizeof(ibv_port_attr));
    if (ibv_query_port(ib_ctx_, config_.ib_port, port_attr_))
    {
        fprintf(stderr, "Fail to query port %u attribute\n", config_.ib_port);
        return 1;
    }
    // Get the channel for cq
    channel_ = ibv_create_comp_channel(ib_ctx_);
    if (!channel_)
    {
      RDMA_ERR_NOTNULL(channel_);
      return 1;
    }
    int flags = fcntl(channel_->fd, F_GETFL);
    int rc = fcntl(channel_->fd, F_SETFL, flags | O_NONBLOCK);
    if (rc < 0)
    {
      fprintf(stderr, "Fail to change file descriper\n");
    }
    // allocate protection domain
    pd_ = ibv_alloc_pd(ib_ctx_);
    if (!pd_)
    {
      RDMA_ERR_NOTNULL(pd_);
      return 1;
    }
    // Create the completion queue
    recvcq_ = ibv_create_cq(ib_ctx_, config_.cq_size_max, NULL, channel_, 0);
    if (!recvcq_)
    {
      RDMA_ERR_NOTNULL(recvcq_);
      return 1;
    }
    // Request recv cq notification
    if (ibv_req_notify_cq(recvcq_, 0))
    {
      fprintf(stderr, "Couldn't request recv CQ notification\n");
      return 1;
    }
    sendcq_ = ibv_create_cq(ib_ctx_, config_.cq_size_max, NULL, NULL, 0);
    if (!sendcq_)
    {
      RDMA_ERR_NOTNULL(sendcq_);
      return 1;
    }
    return 0;
}

int RDMA::ModifyQPToInit(int id)
{
  ibv_qp *qp = qps_[id]->qp;
  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = config_.ib_port;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
  int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  int rc = ibv_modify_qp(qp, &attr, flags);
  if(rc){
    fprintf(stderr, "node%d: failed to modify QP %d to state INIT\n", my_id_, id);
    return rc;
  }
  return 0;
}

int RDMA::ModifyQPToRTR(int id)
{ 
  ibv_qp *qp = qps_[id]->qp;
  const rdma_dest_t& rem_dest = qps_[id]->rem_dest;
  struct ibv_qp_attr attr = {};
  attr.timeout = 12;
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_256;
  attr.dest_qp_num = rem_dest.qp_num;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 0;
  attr.min_rnr_timer = 6;
  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = rem_dest.lid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = config_.ib_port;

  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  int rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) {
    fprintf(stderr, "node%d: failed to modify QP %d to state RTR\n", my_id_, id);
    return rc;
  }
  return 0;
}

int RDMA::ModifyQPToRTS(int id)
{
  ibv_qp *qp = qps_[id]->qp;
  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 12;
  attr.min_rnr_timer = 6;
  attr.sq_psn = 0;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.max_rd_atomic = 0;
  int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
    IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  int rc = ibv_modify_qp(qp, &attr, flags);
  if (rc){
    fprintf(stderr, "node%d: failed to modify QP %d to state RTS\n", my_id_, id);
    return rc;
  }
  return 0;
}

int RDMA::PostRecv(int id)
{
  qp_context_t *qp_ctx = qps_[id];
  struct ibv_sge sge = {};
  struct ibv_recv_wr wr = {};
  struct ibv_recv_wr *bad_wr;
  sge.addr = (uint64_t)qp_ctx->recvbuf;
  sge.length = config_.recvbuf_size;
  sge.lkey = qp_ctx->recvmr->lkey;
  wr.wr_id = 0;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.next = NULL;
  if (ibv_post_recv(qp_ctx->qp, &wr, &bad_wr))
  {
    fprintf(stderr, "node%d: fail to post receive wr for node%d\n", my_id_, id);
    return -1;
  }
  return 0;
}

int RDMA::PostSend(int id, uint64_t send_addr, int length, uint32_t imm_data)
{
  qp_context_t *qp_ctx = qps_[id];
  uint64_t offset = send_addr - (uint64_t)qp_ctx->sendbuf;
  struct ibv_send_wr sr = {};
  struct ibv_sge sge = {};
  struct ibv_send_wr *bad_sr;
  sge.addr = send_addr;
  sge.length = length;
  sge.lkey = qp_ctx->sendmr->lkey;

  int send_flags = IBV_SEND_SOLICITED;
  if (imm_data & 1<<18)
    send_flags = IBV_SEND_SOLICITED | IBV_SEND_SIGNALED;

  sr.next = NULL;
  sr.wr_id = 0;
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  sr.send_flags = send_flags;
  sr.wr.rdma.remote_addr = qp_ctx->rem_dest.recvbuf + (uint64_t)offset;
  sr.wr.rdma.rkey = qp_ctx->rem_dest.rkey;
  sr.imm_data = imm_data;
  
  if(ibv_post_send(qp_ctx->qp, &sr, &bad_sr))
  {
    fprintf(stderr, "node%d: fail to post send wr to node%d\n", my_id_, id);
    return 1;
  }
  //fprintf(stdout, "node%d: succeed to post send wr to node%d, recvbuf addr:%lu, send length:%d, rkey:%lu\n"
  //    , my_id_, id, sr.wr.rdma.remote_addr, sge.length, sr.wr.rdma.rkey);
  return 0;
}
}