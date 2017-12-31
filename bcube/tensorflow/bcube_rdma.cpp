#include "bcube_rdma.h"

#if HAVE_RDMA
#include <rdma/rdma_cma.h>

//void rc_die(const char *reason);
const size_t BUFFER_SIZE = 512 * 1024 * 1024 + 1;
#define TIMEOUT_IN_MS 500
#define TEST_NZ(x) do { if ( (x)) rc_die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) rc_die("error: " #x " failed (returned zero/null)."); } while (0)
#define MIN_CQE 10

#endif // RDMA_SUPPORT

#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <atomic>
#include <unordered_map>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "bcube_utils.h"
#include "bcube_ops.h"
#include "bcube_comm.h"
#include "bcube_message.h"

#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define IS_CLIENT false
#define IS_SERVER true

static std::atomic_bool rdma_server_establisted(false);
static std::atomic_bool rdma_client_establisted(false);

static void rc_die(const char *reason)
{
	extern int errno;
	fprintf(stderr, "%s\nstrerror= %s\n", reason, strerror(errno));
	exit(-1);
}

#if HAVE_RDMA
static void node_counts(bcube_struct& bcube_s)
{
	bcube_s.bcube_node_count = 1;
	for (int lev = 0; lev < bcube_s.bcube_level; lev++)
	{
		bcube_s.bcube_node_count *= bcube_s.bcube0_size;
	}
}

/*确定当前bcube所处的节点和信息*/
static void setup_node(bcube_struct& bcube_s)
{
	char* __rank__ = getenv("BCUBE_RANK");
	if (__rank__ != NULL)bcube_s.rank = atoi(__rank__);
	else
	{
		std::cerr << "error in get env rank, you must set up it before run." << std::endl;
		exit(-1);
	}
	bcube_s.local_info.node_index = bcube_s.rank;
	for (size_t lev = 0; lev < bcube_s.topo.size(); lev++)/*add myself ip into mynodes*/
		bcube_s.local_info.myip.push_back(bcube_s.topo[lev][bcube_s.rank].ip);

	/*发现邻居节点，加入到neighbor_info中*/
	for (int lev = 0; lev < bcube_s.bcube_level; lev++)/*each level*/
	{
		int * tmp_neigh = new int[bcube_s.bcube0_size - 1];
		std::vector<node> grp;
		Utils::getOneHopNeighbour(bcube_s.rank, lev, bcube_s.bcube0_size, 1, tmp_neigh);
		for (int neigh_index = 0; neigh_index < bcube_s.bcube0_size - 1; neigh_index++)
			grp.push_back(bcube_s.topo[lev][tmp_neigh[neigh_index]]);
		bcube_s.neighbor_info.push_back(grp);
		grp.clear();
		delete[] tmp_neigh;
	}
	return;

}

/*加载所有的网络节点*/
/*
192.168.10.XXX
192.168.11.XXX
*/
static void topology_init(bcube_struct& bcube_s)
{
	printf("constructing a BCube(%d,%d) topology\n", bcube_s.bcube0_size, bcube_s.bcube_level);
	node_counts(bcube_s);
	for (int leve = 0; leve < bcube_s.bcube_level; leve++)
	{
		std::string ip_addr = "12.12.";
		std::string leve_str = std::to_string((leve + 10));
		std::vector<node> tp;/*each level*/
		node tmp_node;
		ip_addr += leve_str + std::string(".");

		for (int nodenum = 0; nodenum < bcube_s.bcube_node_count; nodenum++)
		{
			tmp_node.ip = ip_addr + std::to_string(nodenum + 11);
			tmp_node.node_index = nodenum;
			tp.push_back(tmp_node);
		}
		bcube_s.topo.push_back(tp);
	}
	printf("BCube(%d,%d) is constructed done!\n", bcube_s.bcube0_size, bcube_s.bcube_level);
}
struct bcube_global_struct;
static void insert_to_recv_queue(bcube_global_struct& bgs, received_tensor_entry& rs_e)
{
	auto& bs = bgs.bcube_s;
	auto& recv_queue = bgs.receiv_tmp_tensor;
	string name = rs_e.tensor_name;/*point to get tensor_name*/
	auto it = recv_queue.find(name);
	if (it != recv_queue.end())/*exist before and insert into.*/
	{
		//printf("%s exit before, append it behinds, it size is%d\n",name.c_str(),it->second.size());
		auto& vec_msg = it->second;
		vec_msg.push_back(std::move(rs_e));
		if (vec_msg.size() == (size_t)(bs.bcube0_size - 1)*bs.bcube_level)
		{
			/*if all received done, move tensor to received tensor.*/
			//printf("tensor %s is ready to reduce, move to received tensor buf.\n",it->first.c_str());
			{
				std::lock_guard<std::mutex> recv_lock(bgs.bcube_mutex);
				bgs.receiv_tensor.emplace(std::make_pair(name, std::move(vec_msg)));
			}
			recv_queue.erase(it);
		}
	}
	else
	{
		//printf("%s not exist... create one...\n",name.c_str());
		std::vector<received_tensor_entry> msg_record;
		//std::this_thread::sleep_for(std::chrono::seconds(1));
		msg_record.push_back(std::move(rs_e));
		recv_queue.emplace(std::make_pair(name, std::move(msg_record)));
		//printf("insert receive done\n");
	}
	return;
}

#ifdef HAVE_RDMA
static void send_message(struct rdma_cm_id *id)
{
	struct context *ctx = (struct context *)id->context;
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = (uintptr_t)id;
	wr.opcode = IBV_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;

	sge.addr = (uintptr_t)ctx->msg;
	sge.length = sizeof(*ctx->msg);
	sge.lkey = ctx->msg_mr->lkey;

	TEST_NZ(ibv_post_send(id->qp, &wr, &bad_wr));
}

static void send_tensor(struct rdma_cm_id *id, char* buff, uint32_t len)
{
	struct context *ctx = (struct context *)id->context;
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	if (buff)
		memcpy(ctx->buffer, buff, len);
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)id;
	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(len);
	wr.wr.rdma.remote_addr = ctx->peer_addr;
	wr.wr.rdma.rkey = ctx->peer_rkey;
	if (len)
	{
		wr.sg_list = &sge;
		wr.num_sge = 1;

		sge.addr = (uintptr_t)ctx->buffer;
		sge.length = len;
		sge.lkey = ctx->buffer_mr->lkey;
	}
	TEST_NZ(ibv_post_send(id->qp, &wr, &bad_wr));
}

static void post_receive_client(struct rdma_cm_id *id)
{
	struct context *ctx = (struct context *)id->context;
	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)id;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	sge.addr = (uintptr_t)ctx->msg;
	sge.length = sizeof(*ctx->msg);
	sge.lkey = ctx->msg_mr->lkey;
	TEST_NZ(ibv_post_recv(id->qp, &wr, &bad_wr));
}

static void post_receive_server(struct rdma_cm_id *id)
{
	struct ibv_recv_wr wr, *bad_wr = NULL;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)id;
	wr.sg_list = NULL;
	wr.num_sge = 0;
	TEST_NZ(ibv_post_recv(id->qp, &wr, &bad_wr));
}

static char* data_gene(int size)
{
	char* _data = (char*)malloc(size * sizeof(char) + 1);
	_data[size] = 0;
	char padding[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };
	for (int index = 0; index < size; index++)
		_data[index] = padding[index % 10];
	return _data;
}

static char* __send_str = NULL;
static void send_by_RDMA(struct ibv_wc *wc)
{
	struct rdma_cm_id *id = (struct rdma_cm_id *)(uintptr_t)wc->wr_id;
	struct context *ctx = (struct context *)id->context;

	if (wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM)
	{
		printf("send thread %ld will never be here!!!!!\n", pthread_self());
		exit(0);
	}
	else if (wc->opcode & IBV_WC_RECV)
	{
		if (ctx->msg->id == MSG_MR)
		{
			ctx->peer_addr = ctx->msg->data.mr.addr;
			ctx->peer_rkey = ctx->msg->data.mr.rkey;
			printf("received remote memory address and key\n");
			ctx->remote_idle = true;
			printf("thread %ld will send data in 4 seconds\n", pthread_self());
			std::this_thread::sleep_for(std::chrono::seconds(4));

			/*可以发送tensor了，但是去哪拿呢？*/
			__send_str = data_gene(1024 * 1024 * 100);
			send_tensor(id, __send_str, strlen(__send_str));
		}
		else if (ctx->msg->id == MSG_DONE)
		{
			printf("received DONE, disconnecting\n");
			rdma_disconnect(id);
			return;
		}
		else if (ctx->msg->id == MSG_READY)
		{
			ctx->remote_idle = true;
			/*可以发送tensor了，但是去哪拿呢？*/
			printf("thread %ld will send data in 4 seconds\n", pthread_self());
			std::this_thread::sleep_for(std::chrono::seconds(4));
			send_tensor(id, NULL, strlen(__send_str));
		}
		post_receive_client(id);
	}
	return;
}

static void recv_by_RDMA(struct ibv_wc *wc)
{
	struct rdma_cm_id *id = (struct rdma_cm_id *)(uintptr_t)wc->wr_id;
	struct context *ctx = (struct context *)id->context;

	if (wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM)
	{
		uint32_t size = ntohl(wc->imm_data);
		struct sockaddr_in* client_addr = (struct sockaddr_in *)rdma_get_peer_addr(id);
		static int64_t lpop = 0;
		//if (lpop % 500 == 0)
		printf("thread: %ld received %i bytes from client %s!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n", pthread_self(), size, inet_ntoa(client_addr->sin_addr));
		lpop++;
		//printf("%s\n",ctx->buffer);
		post_receive_server(id);
		ctx->msg->id = MSG_READY;
		send_message(id);
	}
	else if (wc->opcode & IBV_WC_RECV)
	{
		printf("recv thread %ld will never be here!!!!!\n", pthread_self());
		exit(0);
	}
	return;
}


static void *recv_poll_cq(void *tmp_id)
{
	struct ibv_cq *cq = NULL;
	struct ibv_wc wc;
	struct rdma_cm_id *id = (struct rdma_cm_id *)tmp_id;
	struct context *ctx = (struct context *)id->context;
	void *ev_ctx = NULL;

	while (1)
	{
		TEST_NZ(ibv_get_cq_event(ctx->comp_channel, &cq, &ev_ctx));
		ibv_ack_cq_events(cq, 1);
		TEST_NZ(ibv_req_notify_cq(cq, 0));

		while (ibv_poll_cq(cq, 1, &wc))
		{
			if (wc.status == IBV_WC_SUCCESS)
			{
				recv_by_RDMA(&wc);
			}
			else
			{
				printf("\nwc = %s\n", ibv_wc_status_str(wc.status));
				rc_die("poll_cq: status is not IBV_WC_SUCCESS");
			}
		}
	}
	return NULL;
}
static void *send_poll_cq(void *tmp_id)
{
	struct ibv_cq *cq = NULL;
	struct ibv_wc wc;
	struct rdma_cm_id *id = (struct rdma_cm_id *)tmp_id;
	struct context *ctx = (struct context *)id->context;
	void *ev_ctx = NULL;

	while (1)
	{
		TEST_NZ(ibv_get_cq_event(ctx->comp_channel, &cq, &ev_ctx));
		ibv_ack_cq_events(cq, 1);
		TEST_NZ(ibv_req_notify_cq(cq, 0));

		while (ibv_poll_cq(cq, 1, &wc))
		{
			if (wc.status == IBV_WC_SUCCESS)
			{
				send_by_RDMA(&wc);
			}
			else
			{
				printf("\nwc = %s\n", ibv_wc_status_str(wc.status));
				rc_die("poll_cq: status is not IBV_WC_SUCCESS");
			}
		}
	}
	return NULL;
}
















static struct ibv_pd * rc_get_pd(struct rdma_cm_id *id)
{
	struct context *ctx = (struct context *)id->context;
	return ctx->pd;
}

static void build_params(struct rdma_conn_param *params)
{
	memset(params, 0, sizeof(*params));

	params->initiator_depth = params->responder_resources = 1;
	params->rnr_retry_count = 7; /* infinite retry */
	params->retry_count = 7;
}

static void build_context(struct rdma_cm_id *id, bool is_server)
{
	struct context *s_ctx = (struct context *)malloc(sizeof(struct context));
	s_ctx->ibv_ctx = id->verbs;
	TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ibv_ctx));
	TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ibv_ctx));
	TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ibv_ctx, MIN_CQE, NULL, s_ctx->comp_channel, 0));
	TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));
	id->context = (void*)s_ctx;
	if (is_server)
	{
		TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, recv_poll_cq, id));
		id->context = (void*)s_ctx;
	}
}

static void build_qp_attr(struct ibv_qp_init_attr *qp_attr, struct rdma_cm_id *id)
{
	struct context *ctx = (struct context *)id->context;
	memset(qp_attr, 0, sizeof(*qp_attr));
	qp_attr->send_cq = ctx->cq;
	qp_attr->recv_cq = ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = 10;
	qp_attr->cap.max_recv_wr = 10;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
}

static void build_connection(struct rdma_cm_id *id, bool is_server)
{
	struct ibv_qp_init_attr qp_attr;
	build_context(id, is_server);
	build_qp_attr(&qp_attr, id);

	struct context *ctx = (struct context *)id->context;
	TEST_NZ(rdma_create_qp(id, ctx->pd, &qp_attr));
}

static void on_pre_conn(struct rdma_cm_id *id, bool is_server)
{
	struct context *ctx = (struct context *)id->context;
	posix_memalign((void **)&ctx->buffer, sysconf(_SC_PAGESIZE), BUFFER_SIZE);
	TEST_Z(ctx->buffer_mr = ibv_reg_mr(rc_get_pd(id), ctx->buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));

	posix_memalign((void **)&ctx->msg, sysconf(_SC_PAGESIZE), sizeof(*ctx->msg));
	TEST_Z(ctx->msg_mr = ibv_reg_mr(rc_get_pd(id), ctx->msg, sizeof(*ctx->msg), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));

	if (is_server)
		post_receive_server(id);
	else
		post_receive_client(id);
}

static void on_connection(struct rdma_cm_id *id)
{
	struct context *ctx = (struct context *)id->context;

	ctx->msg->id = MSG_MR;
	ctx->msg->data.mr.addr = (uintptr_t)ctx->buffer_mr->addr;
	ctx->msg->data.mr.rkey = ctx->buffer_mr->rkey;

	send_message(id);
}

static void on_disconnect(struct rdma_cm_id *id)
{
	struct context *ctx = (struct context *)id->context;

	ibv_dereg_mr(ctx->buffer_mr);
	ibv_dereg_mr(ctx->msg_mr);

	free(ctx->buffer);
	free(ctx->msg);
	free(ctx);
}
static void recv_RDMA(bcube_global_struct& bgs)
{
	bcube_struct& bs = bgs.bcube_s;
	struct rdma_cm_event *event = NULL;
	struct rdma_conn_param cm_params;
	int connecting_client_cnt = 0;
	int client_counts = (bs.bcube0_size - 1) * bs.bcube_level;
	printf("server is inited done (RDMA), waiting for %d client connecting....:)\n", client_counts);
	build_params(&cm_params);

	while (rdma_get_cm_event(bs.event_channel, &event) == 0)
	{
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);

		if (event_copy.event == RDMA_CM_EVENT_CONNECT_REQUEST)
		{
			build_connection(event_copy.id, IS_SERVER);
			on_pre_conn(event_copy.id, IS_SERVER);
			TEST_NZ(rdma_accept(event_copy.id, &cm_params));
		}
		else if (event_copy.event == RDMA_CM_EVENT_ESTABLISHED)
		{
			on_connection(event_copy.id);
			bs.recv_rdma_cm_id.push_back(event_copy.id);

			struct sockaddr_in* client_addr = (struct sockaddr_in *)rdma_get_peer_addr(event_copy.id);
			printf("client[%s,%d] is connecting (RDMA) now... \n", inet_ntoa(client_addr->sin_addr), client_addr->sin_port);
			connecting_client_cnt++;
			if (connecting_client_cnt == client_counts)
				break;
		}
		else if (event_copy.event == RDMA_CM_EVENT_DISCONNECTED)
		{
			rdma_destroy_qp(event_copy.id);
			on_disconnect(event_copy.id);
			rdma_destroy_id(event_copy.id);
			connecting_client_cnt--;
			if (connecting_client_cnt == 0)
				break;
		}
		else
		{
			rc_die("unknown event server\n");
		}
	}
	printf("%d clients have connected to my node (RDMA), ready to receiving loops\n", client_counts);

	int msg_len = sizeof(msg_struct);
	auto& fd_vect = bgs.bcube_s.recv_rdma_cm_id;
	int fd_num = fd_vect.size();
	rdma_server_establisted = true;
	msg_struct msg_buf;
	while (true);
	return;
}
#endif
extern bcube_global_struct bcube_gs;

static void rdma_server_init(bcube_struct& bs)
{
	int init_loops = 0;
	struct sockaddr_in sin;
	printf("init a server (RDMA)....\n");
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;/*ipv4*/
	sin.sin_port = htons(bs.server_port);/*server listen public ports*/
	sin.sin_addr.s_addr = INADDR_ANY;/*listen any connects*/

	TEST_Z(bs.event_channel = rdma_create_event_channel());
	TEST_NZ(rdma_create_id(bs.event_channel, &bs.listener, NULL, RDMA_PS_TCP));

	while (rdma_bind_addr(bs.listener, (struct sockaddr *)&sin))
	{
		std::cerr << "server init failed (RDMA): error in bind socket, will try it again in 2 seconds..." << std::endl;
		if (init_loops > 10)
		{
			rdma_destroy_id(bs.listener);
			rdma_destroy_event_channel(bs.event_channel);
			exit(-1);
		}
		std::this_thread::sleep_for(std::chrono::seconds(2));
		init_loops++;
	}

	int client_counts = (bs.bcube0_size - 1) * bs.bcube_level;
	if (rdma_listen(bs.listener, client_counts))
	{
		std::cerr << "server init failed (RDMA): error in server listening" << std::endl;
		rdma_destroy_id(bs.listener);
		rdma_destroy_event_channel(bs.event_channel);
		exit(-1);
	}

	bcube_gs.bg_thread.push_back(std::thread(recv_RDMA, std::ref(bcube_gs)));

	std::this_thread::sleep_for(std::chrono::seconds(1));
	return;
}



static void rdma_client_init(bcube_struct& bs)
{
	std::cout << "client inited (RDMA) start" << std::endl;
	for (size_t lev = 0; lev < bs.neighbor_info.size(); lev++)
	{
		for (size_t index = 0; index < bs.neighbor_info[lev].size(); index++)
		{
			struct rdma_cm_id *conn = NULL;
			struct rdma_event_channel *ec = NULL;
			std::string local_eth = bs.local_info.myip[lev];/*get each lev ip*/
			struct sockaddr_in ser_in, local_in;/*server ip and local ip*/
			int connect_count = 0;
			memset(&ser_in, 0, sizeof(ser_in));
			memset(&local_in, 0, sizeof(local_in));

			/*bind remote socket*/
			ser_in.sin_family = AF_INET;
			ser_in.sin_port = htons(bs.server_port);/*connect to public port remote*/
			inet_pton(AF_INET, bs.neighbor_info[lev][index].ip.c_str(), &ser_in.sin_addr);

			/*bind local part*/
			local_in.sin_family = AF_INET;
			std::cout << local_eth.c_str() << "----->" << bs.neighbor_info[lev][index].ip.c_str() << std::endl;
			inet_pton(AF_INET, local_eth.c_str(), &local_in.sin_addr);

			TEST_Z(ec = rdma_create_event_channel());
			TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));
			TEST_NZ(rdma_resolve_addr(conn, (struct sockaddr*)(&local_in), (struct sockaddr*)(&ser_in), TIMEOUT_IN_MS));

			struct rdma_cm_event *event = NULL;
			struct rdma_conn_param cm_params;

			build_params(&cm_params);
			while (rdma_get_cm_event(ec, &event) == 0)
			{
				struct rdma_cm_event event_copy;
				memcpy(&event_copy, event, sizeof(*event));
				rdma_ack_cm_event(event);
				if (event_copy.event == RDMA_CM_EVENT_ADDR_RESOLVED)
				{
					build_connection(event_copy.id, IS_CLIENT);
					on_pre_conn(event_copy.id, IS_CLIENT);
					TEST_NZ(rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS));
				}
				else if (event_copy.event == RDMA_CM_EVENT_ROUTE_RESOLVED)
				{
					TEST_NZ(rdma_connect(event_copy.id, &cm_params));
				}
				else if (event_copy.event == RDMA_CM_EVENT_ESTABLISHED)
				{
					struct context *ctx = (struct context *)event_copy.id->context;
					TEST_NZ(pthread_create(&ctx->cq_poller_thread, NULL, send_poll_cq, event_copy.id));
					std::cout << local_eth << " has connected to server[ " << bs.neighbor_info[lev][index].ip << " , " << bs.server_port << " ]" << std::endl;
					break;
				}
				else if (event_copy.event == RDMA_CM_EVENT_REJECTED)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					connect_count++;
					struct context *ctx = (struct context *)event_copy.id->context;
					ibv_dereg_mr(ctx->buffer_mr);
					ibv_dereg_mr(ctx->msg_mr);
					free(ctx->buffer);
					free(ctx->msg);
					free(ctx);
					rdma_destroy_qp(event_copy.id);
					rdma_destroy_id(event_copy.id);
					rdma_destroy_event_channel(ec);
					if (connect_count > 10 * 600)/*after 600 seconds, it will exit.*/
					{
						std::cerr << 600 << "seconds is passed, error in connect to server" << bs.neighbor_info[lev][index].ip << ", check your network condition" << std::endl;
						exit(-1);
					}
					else
					{
						TEST_Z(ec = rdma_create_event_channel());
						TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));
						TEST_NZ(rdma_resolve_addr(conn, (struct sockaddr*)(&local_in), (struct sockaddr*)(&ser_in), TIMEOUT_IN_MS));
					}
				}
				else
				{
					printf("event = %d\n", event_copy.event);
					rc_die("unknown event client\n");
				}
			}
			bs.topo[lev][bs.neighbor_info[lev][index].node_index].send_rdma_event_channel = ec;
			bs.topo[lev][bs.neighbor_info[lev][index].node_index].send_rdma_cm_id = conn;
			bs.neighbor_info[lev][index].send_rdma_event_channel = ec;
			bs.neighbor_info[lev][index].send_rdma_cm_id = conn;
		}
	}
	rdma_client_establisted = true;
	std::cout << "client inited done" << std::endl;
}



static void outputs(bcube_struct& bs, int** sendMatrix, int N, int para_num, int p, int s)
{
#if __outs_all_
	printf("para_num = %d\n", para_num);
#endif
	for (int i = 0; i < N; i++)
	{
		auto& strategy = bs.nodes_send_strategy[i][s][p];
		for (int j = 0; j < N; j++)
		{
			if (-1 != sendMatrix[i][j])
			{
				send_to_one to_one_node;
				to_one_node.node_id = j;
#if __outs_all_
				printf("\nnode%d->node%d, send paraID: %d", i, j, sendMatrix[i][j]);
				for (int cou = 1; cou < para_num; cou++)printf(", %d", sendMatrix[i][j] + cou);
#endif
				for (int col = 0; col < para_num; col++)to_one_node.paraid.push_back(sendMatrix[i][j] + col);
				to_one_node.block_num = para_num;
				strategy.push_back(to_one_node);
			}

		}
#if __outs_all_
		printf("\n");
#endif
	}
}
static void set_sock_fd(bcube_struct& bs)
{
	for (size_t step_index = 0; step_index < bs.my_strategy.size(); step_index++)
	{
		for (size_t proc_index = 0; proc_index < bs.my_strategy[step_index].size(); proc_index++)
		{
			for (size_t node_index = 0; node_index < bs.my_strategy[step_index][proc_index].size(); node_index++)
			{
				auto& each_node = bs.my_strategy[step_index][proc_index][node_index];
				for (size_t lev_index = 0; lev_index < bs.neighbor_info.size(); lev_index++)
				{
					for (size_t neigh_index = 0; neigh_index < bs.neighbor_info[lev_index].size(); neigh_index++)
					{
						if (each_node.node_id == bs.neighbor_info[lev_index][neigh_index].node_index)
							each_node.socket_fd = bs.neighbor_info[lev_index][neigh_index].remote_fd;
					}
				}
			}
		}
	}
}

static void get_send_strategy(bcube_struct& bs)
{
	int n = bs.bcube0_size;
	int k = bs.bcube_level - 1;
	int N = bs.bcube_node_count;
	int para_num;
	int **sendMatrix = new int*[N];

	for (int i = 0; i < N; i++)sendMatrix[i] = new int[N];

	{
		/*resize strategy vector*/
		bs.nodes_send_strategy.resize(N);
		for (size_t node_index = 0; node_index < bs.nodes_send_strategy.size(); node_index++)
		{
			bs.nodes_send_strategy[node_index].resize((k + 1) * 2);
			for (size_t step_index = 0; step_index < bs.nodes_send_strategy[node_index].size(); step_index++)
			{
				bs.nodes_send_strategy[node_index][step_index].resize((k + 1));
			}
		}
	}
	for (int s = 0; s <= k; s++)
	{
		for (int p = 0; p <= k; p++)
		{
#if __outs_all_
			printf("\n\nin scatter stage: %d\t using p %d\n", s, p);
#endif
			Utils::getScatterMatrix(p, s, n, k, N, sendMatrix, para_num);
			outputs(bs, sendMatrix, N, para_num, p, s);
		}

	}
	for (int s = 0; s <= k; s++)
	{
		for (int p = 0; p <= k; p++)
		{
#if __outs_all_
			printf("\n\nin gather stage: %d\t using p %d\n", s, p);
#endif
			Utils::getGatherMatrix(p, s, n, k, N, sendMatrix, para_num);
			outputs(bs, sendMatrix, N, para_num, p, s + k + 1);
		}
	}
	for (int row = 0; row < N; row++)
		delete[] sendMatrix[row];
	delete[] sendMatrix;
	bs.my_strategy = bs.nodes_send_strategy[bs.rank];
	set_sock_fd(bs);
	bs.nodes_send_strategy[bs.rank] = bs.my_strategy;
	//show_each_node(bs, bs.rank);
}


static bool rdma_check_bcube_is_inited_done(bcube_struct& bs)
{
	std::cout << "check bcube inite status, not yet finished" << std::endl;
	return rdma_server_establisted && rdma_client_establisted;
}

/*default is BCube(3,2)*/
void rdma_bcube_init(bcube_struct& bcube_s, bcube_global_struct& bgs)
{
	//signal(SIGINT, sig_handler);
	printf("in rdma bcube init...\n");
	bcube_s.bcube0_size = 3;
	bcube_s.bcube_level = 2;

	topology_init(bcube_s);
	setup_node(bcube_s);
	rdma_server_init(bcube_s);
	rdma_client_init(bcube_s);
	get_send_strategy(bcube_s);
	while (!rdma_check_bcube_is_inited_done(bcube_s))
		std::this_thread::sleep_for(std::chrono::seconds(1));
	return;
}

extern bcube_global_struct bcube_gs;


static void show_msg(void* row_data)
{
	return;
	msg_struct* msg = (msg_struct*)row_data;
	printf("msg info:\n");
	printf("msg_length: %d\n", msg->msg_length);
	printf("name_length: %d\n", msg->name_len);
	printf("start position: %d\n", msg->start_pos);
	printf("msg.data[0]: %c\n", msg->data[0]);
	char* name = (char*)msg + sizeof(msg_struct);
	char* data = name + msg->name_len;
	char tmp = *data;
	*data = 0;
	printf("msg_name: %s\n", name);
	*data = tmp;
	for (int ii = 0; ii < 3; ii++)
		printf("%d ", ((int*)data)[ii]);
	printf("\n");
}
//extern void show_msg(void*);
static void send_assist_thread(tensor_table_entry& a_tensor, process& ps, int pid)
{
	msg_struct* tmp_msg = nullptr;
	auto& tmp_stg = ps[pid];
	for (auto it : tmp_stg)
	{
		int len = 0, res_len = -1;
		tensor_msg::encode(a_tensor, (void**)&tmp_msg, it.paraid[0], it.block_num, &len);
		show_msg((void*)tmp_msg);
		res_len = write(it.socket_fd, (void*)(tmp_msg), len);
		assert(res_len == len);
		std::free(tmp_msg);
		//printf("in send_assist_thread : free %p\n", tmp_msg);
		tmp_msg = nullptr;
	}
}
#include <condition_variable>
static struct thread_assis
{
	std::condition_variable cv;
	std::mutex send_mutex;
	tensor_table_entry e;
	process steps;
	bool ready = false;
	std::vector<bool> fin;
} send_pack;

static bool first_init = true;

static void send_thread(thread_assis& _send_pack, int pid)
{
	while (true)
	{
		printf("run in send thread\n");
		std::unique_lock<std::mutex> send_lock(_send_pack.send_mutex);
		while ((!_send_pack.ready) || _send_pack.fin[pid])_send_pack.cv.wait(send_lock);
		send_assist_thread(_send_pack.e, _send_pack.steps, pid);
		_send_pack.fin[pid] = true;
	}

}
#include <pthread.h>
static void* send_thread_2(void* _pid)
{
	auto pid = *(int*)_pid;
	thread_assis& _send_pack = send_pack;
	while (true)
	{
		std::unique_lock<std::mutex> send_lock(_send_pack.send_mutex);
		while ((!_send_pack.ready) || _send_pack.fin[pid])_send_pack.cv.wait(send_lock);
		send_assist_thread(_send_pack.e, _send_pack.steps, pid);
		_send_pack.fin[pid] = true;
	}
	return NULL;
}

void rdma_bcube_send(tensor_table_entry& e, bcube_struct& bs, int stage)
{
	auto& send_strategy = bs.my_strategy;
	assert((size_t)stage < send_strategy.size());
	auto & steps = send_strategy[stage];
	auto& _send_pack_here = send_pack;

	//std::cout<<"current send is belong to stage "<<stage<<std::endl;
	if (first_init)
	{
		first_init = false;
		_send_pack_here.fin.resize(2);
		for (int nums = 0; nums < 2; nums++)
		{
			pthread_t id1;
			if (pthread_create(&id1, NULL, send_thread_2, (void*)&nums) != 0)
			{
				printf("error in create thread");
				while (1);
			}
			sleep(2);
		}
	}
	std::unique_lock<std::mutex> send_lock(_send_pack_here.send_mutex);
	_send_pack_here.steps = steps;
	_send_pack_here.e = e;
	_send_pack_here.fin[0] = false;
	_send_pack_here.fin[1] = false;
	_send_pack_here.ready = true;
	_send_pack_here.cv.notify_all();
	send_lock.unlock();
	while (_send_pack_here.fin[0] == false || _send_pack_here.fin[1] == false);
	return;
}

#endif