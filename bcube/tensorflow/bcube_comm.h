/*==============================================================================
# Copyright 2017 NEWPLAN, Tsinghua University. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
*/
#ifndef __TENSOTFLOW_BCBUE__
#define __TENSOTFLOW_BCBUE__

#include <vector>
#include <string>
#include "bcube_message.h"
#include "bcube_rdma.h"

typedef struct _data_list_
{
	char* data_ptr = nullptr;
	struct _data_list_* next = nullptr;
} node_item;

struct bcube_global_struct;
struct node
{
	int remote_fd;
	int node_index;
	std::string ip;
	/*below is for server*/
	std::vector<std::string> myip;

#if HAVE_RDMA
	struct rdma_cm_id* send_rdma_cm_id;
	struct rdma_event_channel* send_rdma_event_channel;
	node_item* send_ptr, recv_ptr;
#endif

};

typedef struct
{
	int socket_fd;
	int node_id;
	int block_num;/*block nums should be send once*/
	int block_size;/*each block size*/
	std::vector<int> paraid;
#if HAVE_RDMA
	node_item* send_ptr, recv_ptr;
#endif
} send_to_one;

/*
D1:node index;
D2:stage index;
D3��process index;
*/
typedef std::vector<send_to_one> send_strategy;
typedef std::vector<send_strategy> process;
typedef std::vector<process> step;


struct bcube_struct
{
	int bcube0_size;/*node count in bcube0*/
	int bcube_level;/*level count in bcube*/
	int bcube_node_count;

	int rank;

	int server_fd;/*server listen fd*/
	int server_port = 9610; /*public port*/

	std::vector<int> recv_fd;/*recv socket fd*/
	std::vector<std::vector<node>> topo, neighbor_info;

	node local_info;/*local server socket, will be free after initilization*/

	std::vector<step> nodes_send_strategy;/*global send strategy*/
	std::vector<process> my_strategy;/*strategy for current rank*/

#if HAVE_RDMA
	struct rdma_event_channel *event_channel;
	struct rdma_cm_id *listener;
	std::vector<rdma_cm_id*> recv_rdma_cm_id;
#endif

};

void bcube_init(bcube_struct&, bcube_global_struct&);
void bcube_send(tensor_table_entry& , bcube_struct& , int );

#endif
