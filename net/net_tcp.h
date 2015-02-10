/*
 * Copyright (C) 2015 OpenSIPS Project
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *
 * history:
 * ---------
 *  2015-01-xx  created (razvanc)
 */

#ifndef _NET_TCP_H_
#define _NET_TCP_H_

#include "../mi/mi.h"
#include "tcp_conn_defs.h"

#define TCP_PARTITION_SIZE 32

/**************************** Control functions ******************************/

/* initializes the TCP structures */
int tcp_init(void);

/* destroys the TCP data */
void tcp_destroy(void);

/* checks if the TCP layer may provide async write support */
int tcp_has_async_write(void);

/* creates the communication channel between a generic OpenSIPS process
   and the TCP MAIN process - TO BE called before forking */
int tcp_pre_connect_proc_to_tcp_main(void);

/* same as above, but to be called after forking, both in child and parent */
void tcp_connect_proc_to_tcp_main( int chid );

/* tells how many processes the TCP layer will create */
int tcp_count_processes(void);

/* starts all TCP related processes */
int tcp_start_processes(int *chd_rank, int *startup_done);

/* MI function to list all existing TCP connections */
struct mi_root *mi_tcp_list_conns(struct mi_root *cmd, void *param);

/********************** TCP conn management functions ************************/

/* initializes an already defined TCP listener */
int tcp_init_listener(struct socket_info *si);

/* helper function to set all TCP related options to a socket */
int tcp_init_sock_opt(int s);

// used to return a listener
struct socket_info* tcp_find_listener(union sockaddr_union* to, int proto);

/* returns the connection identified by either the id or the destination to */
int tcp_conn_get(int id, struct ip_addr* ip, int port, int timeout,
		struct tcp_connection** conn, int* conn_fd);

/* creates a new tcp conn around a newly connected socket */
struct tcp_connection* tcp_conn_create(int sock, union sockaddr_union* su,
		struct socket_info* ba, int type, int state);

/* release a connection aquired via tcp_conn_get() or tcp_conn_create() */
void tcp_conn_release(struct tcp_connection* c, int pending_data);

// used to tune the connection attributes
int tcp_conn_fcntl(struct tcp_connection *conn, int attr, void *value);

#endif /* _NET_TCP_H_ */