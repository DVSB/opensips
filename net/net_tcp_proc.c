/*
 * Copyright (C) 2013-2014 OpenSIPS Solutions
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 * 2015-02-05 imported from former tpc_read.c (bogdan)
 */


#include "../timer.h"
#include "../reactor.h"
#include "tcp_conn.h"
#include "tcp_passfd.h"
#include "net.h"


/*!< list of tcp connections handled by this process */
static struct tcp_connection* tcp_conn_lst=0;

static int tcpmain_sock=-1;


static void tcpconn_release(struct tcp_connection* c, long state)
{
	long response[2];

	LM_DBG(" releasing con %p, state %ld, fd=%d, id=%d\n",
			c, state, c->fd, c->id);
	LM_DBG(" extra_data %p\n", c->extra_data);

	if (c->con_req) {
		pkg_free(c->con_req);
		c->con_req = NULL;
	}

	/* release req & signal the parent */
	if (c->fd!=-1) close(c->fd);
	/* errno==EINTR, EWOULDBLOCK a.s.o todo */
	response[0]=(long)c;
	response[1]=state;
	if (send_all(tcpmain_sock, response, sizeof(response))<=0)
		LM_ERR("send_all failed\n");
}


/* wrapper around internal tcpconn_release() - to be called by functions which
 * used tcp_conn_get(), in order to release the connection;
 * It does the unref and pushes back (if needed) some update to TCP main;
 * right now, it used only from the xxx_send() functions */
void tcp_conn_release(struct tcp_connection* c, int pending_data)
{
	if (c->state==S_CONN_BAD) {
		c->timeout=0;
		/* CONN_ERROR will auto-dec refcnt => we must not call tcpconn_put !!*/
		tcpconn_release(c, CONN_ERROR);
		return;
	}
	if (pending_data)
		tcpconn_release(c, ASYNC_WRITE);
	tcpconn_put(c);
	return;
}


/*! \brief
 *  handle io routine, based on the fd_map type
 * (it will be called from reactor_main_loop )
 * params:  fm  - pointer to a fd hash entry
 *          idx - index in the fd_array (or -1 if not known)
 * return: -1 on error, or when we are not interested any more on reads
 *            from this fd (e.g.: we are closing it )
 *          0 on EAGAIN or when by some other way it is known that no more
 *            io events are queued on the fd (the receive buffer is empty).
 *            Usefull to detect when there are no more io events queued for
 *            sigio_rt, epoll_et, kqueue.
 *         >0 on successfull read from the fd (when there might be more io
 *            queued -- the receive buffer might still be non-empty)
 */
inline static int handle_io(struct fd_map* fm, int idx,int event_type)
{
	int ret=0;
	int n;
	struct tcp_connection* con;
	int s,rw;
	long resp;
	long response[2];

	switch(fm->type){
		case F_TIMER_JOB:
			handle_timer_job();
			break;
		case F_SCRIPT_ASYNC:
			async_resume_f( fm->fd, fm->data);
			return 0;
		case F_TCPMAIN:
again:
			ret=n=receive_fd(fm->fd, response, sizeof(response), &s, 0);
			if (n<0){
				if (errno == EWOULDBLOCK || errno == EAGAIN){
					ret=0;
					break;
				}else if (errno == EINTR) goto again;
				else{
					LM_CRIT("read_fd: %s \n", strerror(errno));
						abort(); /* big error*/
				}
			}
			if (n==0){
				LM_WARN("0 bytes read\n");
				break;
			}
			con = (struct tcp_connection *)response[0];
			rw = (int)response[1];

			if (con==0){
					LM_CRIT("null pointer\n");
					break;
			}
			con->fd=s;
			if (s==-1) {
				LM_ERR("read_fd:no fd read\n");
				goto con_error;
			}
			if (con==tcp_conn_lst){
				LM_CRIT("duplicate"
							" connection received: %p, id %d, fd %d, refcnt %d"
							" state %d (n=%d)\n", con, con->id, con->fd,
							con->refcnt, con->state, n);
				tcpconn_release(con, CONN_ERROR);
				break; /* try to recover */
			}

			LM_DBG("We have received conn %p with rw %d\n",con,rw);
			if (rw & IO_WATCH_READ) {
				/* 0 attempts so far for this SIP MSG */
				con->msg_attempts = 0;

				/* must be before reactor_add, as the add might catch some
				 * already existing events => might call handle_io and
				 * handle_io might decide to del. the new connection =>
				 * must be in the list */
				tcpconn_listadd(tcp_conn_lst, con, c_next, c_prev);
				con->timeout=get_ticks()+tcp_max_msg_time;
				if (reactor_add_reader( s, F_TCPCONN, con )<0) {
					LM_CRIT("failed to add new socket to the fd list\n");
					tcpconn_listrm(tcp_conn_lst, con, c_next, c_prev);
					goto con_error;
				}
			} else if (rw & IO_WATCH_WRITE) {
				LM_DBG("Received con %p ref = %d\n",con,con->refcnt);
				lock_get(&con->write_lock);
				resp = proto_net_binds[con->type].write( (void*)con );
				lock_release(&con->write_lock);
				if (resp<0) {
					ret=-1; /* some error occured */
					con->state=S_CONN_BAD;
					tcpconn_release(con, CONN_ERROR);
					break;
				} else if (resp==1) {
					tcpconn_release(con, ASYNC_WRITE);
				} else {
					tcpconn_release(con, CONN_RELEASE);
				}
				ret = 0;
			}
			break;
		case F_TCPCONN:
			if (event_type & IO_WATCH_READ) {
				con=(struct tcp_connection*)fm->data;
				resp = proto_net_binds[con->type].read( (void*)con, &ret );
				reactor_del_all( con->fd, idx, IO_FD_CLOSING );
				tcpconn_listrm(tcp_conn_lst, con, c_next, c_prev);
				if (resp<0) {
					ret=-1; /* some error occured */
					con->state=S_CONN_BAD;
					tcpconn_release(con, CONN_ERROR);
					break;
				} else if (con->state==S_CONN_EOF) {
					tcpconn_release(con, CONN_EOF);
				} else {
					tcpconn_release(con, CONN_RELEASE);
				}
			}
			break;
		case F_NONE:
			LM_CRIT("empty fd map %p: "
						"{%d, %d, %p}\n", fm,
						fm->fd, fm->type, fm->data);
			goto error;
		default:
			LM_CRIT("uknown fd type %d\n", fm->type);
			goto error;
	}

	return ret;
con_error:
	con->state=S_CONN_BAD;
	tcpconn_release(con, CONN_ERROR);
	return ret;
error:
	return -1;
}



/*! \brief  releases expired connections and cleans up bad ones (state<0) */
static inline void tcp_receive_timeout(void)
{
	struct tcp_connection* con;
	struct tcp_connection* next;
	unsigned int ticks;

	ticks=get_ticks();
	for (con=tcp_conn_lst; con; con=next) {
		next=con->c_next; /* safe for removing */
		if (con->state<0){   /* kill bad connections */
			/* S_CONN_BAD or S_CONN_ERROR, remove it */
			/* fd will be closed in tcpconn_release */
			reactor_del_reader(con->fd, -1/*idx*/, IO_FD_CLOSING/*io_flags*/ );
			tcpconn_listrm(tcp_conn_lst, con, c_next, c_prev);
			con->state=S_CONN_BAD;
			tcpconn_release(con, CONN_ERROR);
			continue;
		}
		if (con->timeout<=ticks){
			LM_DBG("%p expired - (%d, %d) lt=%d\n",
					con, con->timeout, ticks,con->lifetime);
			/* fd will be closed in tcpconn_release */
			reactor_del_reader(con->fd, -1/*idx*/, IO_FD_CLOSING/*io_flags*/ );
			tcpconn_listrm(tcp_conn_lst, con, c_next, c_prev);
			if (con->msg_attempts)
				tcpconn_release(con, CONN_ERROR);
			else
				tcpconn_release(con, CONN_RELEASE);
		}
	}
}



void tcp_worker_proc( int unix_sock, int max_fd )
{
	/* init reactor for TCP worker */
	tcpmain_sock=unix_sock; /* init com. socket */
	if ( init_worker_reactor( "TCP_worker", max_fd)<0 ) {
		goto error;
	}

	/* start watching for the timer jobs */
	if (reactor_add_reader( timer_fd_out, F_TIMER_JOB, NULL)<0) {
		LM_CRIT("failed to add timer pipe_out to reactor\n");
		goto error;
	}

	/* add the unix socket */
	if (reactor_add_reader( tcpmain_sock, F_TCPMAIN, NULL)<0) {
		LM_CRIT("failed to add socket to the fd list\n");
		goto error;
	}

	/* main loop */
	reactor_main_loop( TCP_CHILD_SELECT_TIMEOUT, error, tcp_receive_timeout());

error:
	destroy_worker_reactor();
	LM_CRIT("exiting...");
	exit(-1);
}

