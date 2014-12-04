/*
   drbd_transport_rdma.c

   This file is part of DRBD.

   Copyright (C) 2014, LINBIT HA-Solutions GmbH.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/module.h>
#include <drbd_transport.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include "drbd_int.h"

/* RCK: hack, remove it after connection logic is implemented */
#include <linux/moduleparam.h>
bool rdma_server;
module_param(rdma_server, bool, 0644);

/* RCK:XXX/TODOs:
 * HIGH-LEVEL DESIGN:
 * - discuss what semantics we want to support. There is an interesting read:
 *   http://www.google.at/url?sa=t&rct=j&q=&esrc=s&source=web&cd=1&ved=0CCEQFjAA&url=http%3A%2F%2Fwww.mellanox.com%2Fpdf%2Fwhitepapers%2FWP_Why_Compromise_10_26_06.pdf&ei=VwmHVPXjNsOwPJqcgdgG&usg=AFQjCNFpc5OYdd-h8ylNRUhJjhsILCsZhw&sig2=8MbEQtzOPLpgmL36q6t48Q&bvm=bv.81449611,d.ZWU&cad=rja
 *   google: "rdma infiniband difference send write"
 *   page 5, data transfer semantics. Surprisingly, this favours send/receive.
 *   My limited experience: send/receive is easier, eg. no need to exchange the
 *   rkey. If they are equally fast and rdma write/read is not supported on all
 *   devices, maybe we should stick - at least for the moment - with the easier
 *   to implement send/receive paradigm.
 *
 * IMPLEMENTATION QUIRKS
 * - Connection logic: implement waiter/listener logic. Currently client/server
 *   are set _pre_ compiletime. urgs
 * - we have to make sure we poste new rx_descs (guess that one got removed
 *   from my dummy-test example, re-add it!
 * - I DO NOT FREE the mallocs AT ALL, currently the overall runtime will be very limited ;-)
 */

MODULE_AUTHOR("Roland Kammerer <roland.kammerer@linbit.com>");
MODULE_AUTHOR("Foo Bar <foo.bar@linbit.com>");
MODULE_DESCRIPTION("RDMA transport layer for DRBD");
MODULE_LICENSE("GPL");

/* #define RDMA_MAX_RX 1024 */
/* #define RDMA_MAX_TX 1024 */
#define RDMA_MAX_RX 20
#define RDMA_MAX_TX 20
#define RDMA_PAGE_SIZE 4096

enum drbd_rdma_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	DISCONNECTED,
	ERROR
};

/* RCK: in a next step we should think about the sizes, eg post smaller
 * rx_descs for contol data */
struct drbd_rdma_rx_desc {
	char data[RDMA_PAGE_SIZE];
	void *pos;
	u64 dma_addr;
	struct ib_sge sge;
	unsigned long xfer_len;
} __attribute__((packed));


struct drbd_rdma_tx_desc {
	void *data;
	struct ib_sge sge;
};

struct drbd_rdma_stream {
	struct rdma_cm_id *cm_id;
	struct rdma_cm_id *child_cm_id; /* RCK: no clue what this is for... */

	struct ib_cq *recv_cq;
	struct ib_cq *send_cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct ib_mr *dma_mr;

	enum drbd_rdma_state state;
	wait_queue_head_t sem_state;
	wait_queue_head_t sem_recv;

	atomic_t post_send_count;
	int post_recv_count;

	unsigned long recv_timeout;
};

struct drbd_rdma_transport {
	struct drbd_transport transport;

	/* RCK: in contrast to the transport_tcp I do not split them into two
	 * individual structs (one for data, one for control). I guess it is nicer
	 * to use something like:
	 * tr->stream[DATA_STREAM]
	 *
	 * Actually it is nice, maybe do the same for transport_tcp */

	struct drbd_rdma_stream *stream[2];
};

struct dtr_listener {
	struct drbd_listener listener;
	/* xxx */
};

struct dtr_waiter {
	struct drbd_waiter waiter;
	/* xxx */
};

static struct drbd_transport *dtr_create(struct drbd_connection *connection);
static void dtr_free(struct drbd_transport *transport, enum drbd_tr_free_op);
static int dtr_connect(struct drbd_transport *transport);
static int dtr_recv(struct drbd_transport *transport, enum drbd_stream stream, void **buf, size_t size, int flags);
static void dtr_stats(struct drbd_transport *transport, struct drbd_transport_stats *stats);
static void dtr_set_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream, long timeout);
static long dtr_get_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream);
static int dtr_send_page(struct drbd_transport *transport, enum drbd_stream stream, struct page *page,
		int offset, size_t size, unsigned msg_flags);
static int dtr_recv_pages(struct drbd_peer_device *peer_device, struct page **page, size_t size);
static bool dtr_stream_ok(struct drbd_transport *transport, enum drbd_stream stream);
static bool dtr_hint(struct drbd_transport *transport, enum drbd_stream stream, enum drbd_tr_hints hint);


static struct drbd_transport_class rdma_transport_class = {
	.name = "rdma",
	.create = dtr_create,
	.list = LIST_HEAD_INIT(rdma_transport_class.list),
};

static struct drbd_transport_ops dtr_ops = {
	.free = dtr_free,
	.connect = dtr_connect,
	.recv = dtr_recv,
	.stats = dtr_stats,
	.set_rcvtimeo = dtr_set_rcvtimeo,
	.get_rcvtimeo = dtr_get_rcvtimeo,
	.send_page = dtr_send_page,
	.recv_pages = dtr_recv_pages,
	.stream_ok = dtr_stream_ok,
	.hint = dtr_hint,
};


static struct drbd_transport *dtr_create(struct drbd_connection *connection)
{
	struct drbd_rdma_transport *rdma_transport;

	if (!try_module_get(THIS_MODULE))
		return NULL;

	rdma_transport = kzalloc(sizeof(struct drbd_rdma_transport), GFP_KERNEL);
	if (!rdma_transport) {
		module_put(THIS_MODULE);
		return NULL;
	}

	rdma_transport->transport.ops = &dtr_ops;
	rdma_transport->transport.connection = connection;

	return &rdma_transport->transport;
}

static void dtr_free(struct drbd_transport *transport, enum drbd_tr_free_op free_op)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	/* TODO: shutdown the connection */

	if (free_op == DESTROY_TRANSPORT) {
		kfree(rdma_transport);
		module_put(THIS_MODULE);
	}
}


static int dtr_create_and_post_tx_desc(struct drbd_rdma_stream *, enum drbd_stream, void *, size_t, unsigned);

static int dtr_send(struct drbd_transport *transport, enum drbd_stream stream, void *buf, size_t size, unsigned msg_flags)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	struct drbd_rdma_stream *rdma_stream = rdma_transport->stream[stream];

	if (stream == CONTROL_STREAM) {
		printk("send with CONTROL_STREAM\n");
	}
	else if (stream == DATA_STREAM){
		printk("send with DATA_STREAM\n");
	} else {
		printk("send with unknown STREAM!!!\n");
	}
	printk("send with data[0]:%x\n", ((char*)buf)[0]);
	dtr_create_and_post_tx_desc(rdma_stream, stream, buf, size, msg_flags);

	return size;
}

static int dtr_drain_rx_control_cq(struct drbd_rdma_stream *, struct drbd_rdma_rx_desc **, int);
static int dtr_drain_rx_data_cq(struct drbd_rdma_stream *, struct drbd_rdma_rx_desc **, int);

static int dtr_recv_pages(struct drbd_peer_device *peer_device, struct page **page, size_t size)
{
	printk("not implemented yet\n");
	return 0;
}

static int dtr_create_and_post_rx_desc(struct drbd_rdma_stream *rdma_stream);

static int dtr_recv(struct drbd_transport *transport, enum drbd_stream stream, void **buf, size_t size, int flags)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	struct drbd_rdma_stream *rdma_stream = rdma_transport->stream[stream];

	/* make it member of transport */
	static struct drbd_rdma_rx_desc *old_rx[2] = {NULL, NULL};

	struct drbd_rdma_rx_desc *rx_desc[2] = {NULL, NULL};
	int completed_tx = 0;
	void *buffer;
 	int (*dtr_drain_rx_cq)(struct drbd_rdma_stream *, struct drbd_rdma_rx_desc **, int);

	dtr_drain_rx_cq = NULL;
	if (stream == CONTROL_STREAM) {
		printk("recv with CONTROL_STREAM, size:%d\n", size);
		dtr_drain_rx_cq = dtr_drain_rx_control_cq;
	}
	else if (stream == DATA_STREAM) {
		printk("recv with DATA_STREAM, size:%d\n", size);
		dtr_drain_rx_cq = dtr_drain_rx_data_cq;
	} else {
		printk("recv with unknown STREAM\n");
	}

	if (!(flags & GROW_BUFFER)) {
		/* free/recycle the old_rx[stream] */
		/* currently this should include: */
		;
	}

	if (flags & GROW_BUFFER) {
		printk("RDMA: recv GROW_BUFFER\n");
		/* D_ASSERT(transport->connection, *buf == tcp_transport->rbuf[stream].base); */
		buffer = old_rx[stream]->pos;
		old_rx[stream]->pos += size;
		/* D_ASSERT(transport->connection, (buffer - *buf) + size <= PAGE_SIZE); */
		*buf = buffer;
		/* old_rx[stream] = NULL; */
	} else {
		if ( (old_rx[stream] == NULL) || (old_rx[stream]->xfer_len == 0) ) { /* get a completely new entry */
			int t;
			printk("RDMA: recv completely new on %s\n", stream == CONTROL_STREAM ? "control": "data");
retry_new:
			printk("wating for %lld\n", rdma_stream->recv_timeout);
			/* t = wait_event_interruptible_timeout(rdma_stream->sem_recv, */
			/* 			dtr_drain_rx_cq(rdma_stream, &rx_desc[stream], 1), */
			/* 			rdma_stream->recv_timeout); */
			t = wait_event_interruptible_timeout(rdma_stream->sem_recv,
					dtr_drain_rx_cq(rdma_stream, &rx_desc[stream], 1),
					10000*HZ);

			if (t <= 0)
			{
				if (t==0)
					printk("RDMA: recv() timed out, ret: EAGAIN\n");
				else
					printk("RDMA: recv() timed out, ret: EINTR\n");
				return t == 0 ? -EAGAIN : -EINTR;
			}

			buffer = rx_desc[stream]->data;
			rx_desc[stream]->pos = buffer + size;
			printk("got a new page with size: %d\n", rx_desc[stream]->xfer_len);
			if (rx_desc[stream]->xfer_len < size)
				printk("new, requesting more (%d) than left (%d)\n", size, rx_desc[stream]->xfer_len);
			rx_desc[stream]->xfer_len -= size;
			old_rx[stream] = rx_desc[stream];

			if (flags & CALLER_BUFFER) {
				printk("doing a memcpy on first\n");
				memcpy(*buf, buffer, size);
			}
			else
				*buf = buffer;

			printk("RDMA: recv completely new fine, returning size on %s\n", stream == CONTROL_STREAM ? "control": "data");
			return size;
		} else { /* return next part */
			printk("RDMA: recv next part on %s\n", stream == CONTROL_STREAM ? "control": "data");
			buffer = old_rx[stream]->pos;
			old_rx[stream]->pos += size;

			if (old_rx[stream]->xfer_len <= size) { /* < could be a problem, right? or does that happen? */
				old_rx[stream]->xfer_len = 0; /* 0 left == get new entry */
				printk("marking page as consumed\n");
			}
			else {
				old_rx[stream]->xfer_len -= size;
				printk("old_rx left: %d\n", old_rx[stream]->xfer_len);
			}

			if (flags & CALLER_BUFFER) {
				printk("doing a memcpy on next\n");
				memcpy(*buf, buffer, size);
			}
			else
				*buf = buffer;

			printk("RDMA: recv next part fine, returning size on %s\n", stream == CONTROL_STREAM ? "control": "data");
			return size;
		}
	}

	/* RCK: of course we need a better strategy, but for now, just add a new rx_desc if we consumed one... */
	dtr_create_and_post_rx_desc(rdma_stream);

	return 0;
}

static void dtr_stats(struct drbd_transport* transport, struct drbd_transport_stats *stats)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	/* RCK: first two for debugfs, for now do not care */
	stats->unread_received = 0;
	stats->unacked_send = 0;

	/* RCK: these are used by the sender, guess we should them get right */
	stats->send_buffer_size = RDMA_MAX_TX;
	stats->send_buffer_used = atomic_read(&(rdma_transport->stream[DATA_STREAM]->post_send_count));
}

static int dtr_cma_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
	int err;
	/* context comes from rdma_create_id() */
	struct drbd_rdma_stream *rdma_stream = cm_id->context;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		printk("RDMA(cma event): addr resolved\n");
		rdma_stream->state = ADDR_RESOLVED;
		err = rdma_resolve_route(cm_id, 2000);
		if (err) {
			printk("RDMA: rdma_resolve_route error %d\n", err);
			wake_up_interruptible(&rdma_stream->sem_state);
		}
		else {
			printk("RDMA: rdma_resolve_route OK\n");
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		printk("RDMA(cma event): route resolved\n");
		rdma_stream->state = ROUTE_RESOLVED;
		wake_up_interruptible(&rdma_stream->sem_state);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		printk("RDMA(cma event): connect request\n");
		/* for listener */
		rdma_stream->state = CONNECT_REQUEST;
#if 1
		/* RCK: this is from the contribution, currently I do not see a need for it,
		 * but I keep "child_cm_id" in the struct for now */

		rdma_stream->child_cm_id = cm_id;
		printk("RDMA: child cma %p\n", rdma_stream->child_cm_id);
#endif
		wake_up_interruptible(&rdma_stream->sem_state);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		printk("RDMA(cma event): established\n");
		rdma_stream->state = CONNECTED;
		wake_up_interruptible(&rdma_stream->sem_state);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
		printk("RDMA(cma event, err): ADDR_ERROR\n");
	case RDMA_CM_EVENT_ROUTE_ERROR:
		printk("RDMA(cma event, err): ADDR_ROUTE_ERROR\n");
	case RDMA_CM_EVENT_CONNECT_ERROR:
		printk("RDMA(cma event, err): ADDR_CONNECT_ERROR\n");
	case RDMA_CM_EVENT_UNREACHABLE:
		printk("RDMA(cma event, err): ADDR_UNREACHABLE\n");
	case RDMA_CM_EVENT_REJECTED:
		printk("RDMA(cma event, err): ADDR_REJECTED\n");
		printk("RDMA(cma event: bad thingy, fall-through, only first valid) %d, error %d\n", event->event,
			event->status);
		rdma_stream->state = ERROR;
		wake_up_interruptible(&rdma_stream->sem_state);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk("RDMA(cma event) disconnect event\n");
		rdma_stream->state = DISCONNECTED;
		if ((rdma_stream->post_recv_count == 0) &&
		    (atomic_read(&rdma_stream->post_send_count) == 0))
			wake_up_interruptible(&rdma_stream->sem_state);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk("RDMA(cma event): detected device removal!\n");
		break;

	default:
		printk("RDMA(cma event): oof bad type!\n");
		wake_up_interruptible(&rdma_stream->sem_state);
		break;
	}
	return 0;
}

static int dtr_create_cm_id(struct drbd_rdma_stream *rdma_stream)
{

	rdma_stream->state = IDLE;
	init_waitqueue_head(&rdma_stream->sem_state);
	init_waitqueue_head(&rdma_stream->sem_recv);

	/* create CM id */
	rdma_stream->cm_id = rdma_create_id(
				dtr_cma_event_handler,
				rdma_stream, RDMA_PS_TCP, IB_QPT_RC);

#if 0 /* maybe add this tpye */
	drbd_info(rdma_transport, "RDMA: new cm id %p\n", rdma_transport->cm_id);
#else
	printk("RDMA: new cm id %p\n", rdma_stream->cm_id);
#endif
	if (!rdma_stream->cm_id) {
		return -ENOMEM;
	}

	return 0;
}

#if 0
/* RCK: do we need the following two functions twice (control/data)? */
static void dtr_rx_completion(struct drbd_rdma_stream *rdma_stream,
		struct drbd_rdma_rx_desc *desc, unsigned long xfer_len)
{
	ib_dma_sync_single_for_cpu(rdma_stream->cm_id->device, desc->dma_addr,
			RDMA_PAGE_SIZE, DMA_FROM_DEVICE);
	rdma_stream->post_recv_count--;

	printk("got buffer[0]: %x\n", desc->data[0]);
}
#endif

/* receive max nr_elements (currently should always be used with "1"
 * if -1: receive all
 * >= 0 : nr_elements
 * number of elements in cq is too small to underflow nr_elements */
static int dtr_drain_rx_control_cq(struct drbd_rdma_stream *rdma_stream, struct drbd_rdma_rx_desc **rx_desc, int nr_elements)
{
	struct ib_cq *cq = rdma_stream->recv_cq;
	struct ib_wc wc;
	int completed_tx = 0;
	unsigned long xfer_len;

	while (nr_elements-- && (ib_poll_cq(cq, 1, &wc) == 1)) {
		*rx_desc = (struct drbd_rdma_rx_desc *) (unsigned long) wc.wr_id;
		WARN_ON(rx_desc == NULL);

		if(wc.status == IB_WC_SUCCESS) {
			printk("RDMA: IB_WC_SUCCESS\n");
			if (wc.opcode == IB_WC_SEND)
				printk("RDMA: IB_WC_SEND\n");
			if (wc.opcode == IB_WC_RECV) {
				printk("RDMA: IB_WC_RECV\n");
				xfer_len = (unsigned long)wc.byte_len;
				printk("RDMA: xfer_len: %lu\n", xfer_len);
				/* dtr_rx_completion(rdma_stream, desc, xfer_len); */
				ib_dma_sync_single_for_cpu(rdma_stream->cm_id->device, (*rx_desc)->dma_addr,
						RDMA_PAGE_SIZE, DMA_FROM_DEVICE);
				rdma_stream->post_recv_count--;
				(*rx_desc)->xfer_len = xfer_len;
				printk("in drain (control): %p, data[0]:%x\n", rx_desc, (*rx_desc)->data[0]);
			}
			else if (wc.opcode == IB_WC_RDMA_WRITE)
				printk("RDMA: IB_WC_RDMA_WRITE\n");
			else if (wc.opcode == IB_WC_RDMA_READ)
				printk("RDMA: IB_WC_RDMA_READ\n");
			else
				printk("RDMA: WC SUCCESS, but strange opcode...\n");

			completed_tx++;
		}
		else
			printk("RDMA: IB_WC NOT SUCCESS\n");
	}

out:
	printk("rx completed: %d\n", completed_tx);
	/* ib_req_notify_cq(cq, IB_CQ_NEXT_COMP); */

	printk("returning from drain\n");
	return completed_tx;
}

static int dtr_drain_rx_data_cq(struct drbd_rdma_stream *rdma_stream, struct drbd_rdma_rx_desc **rx_desc, int nr_elements)
{
	struct ib_cq *cq = rdma_stream->recv_cq;
	struct ib_wc wc;
	int completed_tx = 0;
	unsigned long xfer_len;

	while (nr_elements-- && (ib_poll_cq(cq, 1, &wc) == 1)) {
		*rx_desc = (struct drbd_rdma_rx_desc *) (unsigned long) wc.wr_id;
		WARN_ON(rx_desc == NULL);

		if(wc.status == IB_WC_SUCCESS) {
			printk("RDMA: IB_WC_SUCCESS\n");
			if (wc.opcode == IB_WC_SEND)
				printk("RDMA: IB_WC_SEND\n");
			if (wc.opcode == IB_WC_RECV) {
				printk("RDMA: IB_WC_RECV\n");
				xfer_len = (unsigned long)wc.byte_len;
				printk("RDMA: xfer_len: %lu\n", xfer_len);
				/* dtr_rx_completion(rdma_stream, desc, xfer_len); */
				ib_dma_sync_single_for_cpu(rdma_stream->cm_id->device, (*rx_desc)->dma_addr,
						RDMA_PAGE_SIZE, DMA_FROM_DEVICE);
				rdma_stream->post_recv_count--;
				(*rx_desc)->xfer_len = xfer_len;
				printk("in drain (data): %p, data[0]:%x\n", rx_desc, (*rx_desc)->data[0]);
			}
			else if (wc.opcode == IB_WC_RDMA_WRITE)
				printk("RDMA: IB_WC_RDMA_WRITE\n");
			else if (wc.opcode == IB_WC_RDMA_READ)
				printk("RDMA: IB_WC_RDMA_READ\n");
			else
				printk("RDMA: WC SUCCESS, but strange opcode...\n");

			completed_tx++;
		}
		else
			printk("RDMA: IB_WC NOT SUCCESS\n");
	}

out:
	printk("rx completed: %d\n", completed_tx);
	/* ib_req_notify_cq(cq, IB_CQ_NEXT_COMP); */

	printk("returning from drain\n");
	return completed_tx;
}

static void dtr_rx_control_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct drbd_rdma_stream *rdma_stream = ctx;
	int ret;

	printk("RDMA (control): got rx cq event. state %d\n", rdma_stream->state);

	/* dtr_drain_rx_cq(rdma_stream, 1); */

	wake_up_interruptible(&rdma_stream->sem_recv);
	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	if (ret)
		printk("ib_req_notify_cq failed\n");
	else
		printk("ib_req_notify_cq success\n");
}

static void dtr_rx_data_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct drbd_rdma_stream *rdma_stream = ctx;
	int ret;

	printk("RDMA (data): got rx cq event. state %d\n", rdma_stream->state);

	/* dtr_drain_rx_cq(rdma_stream, 1); */

	wake_up_interruptible(&rdma_stream->sem_recv);
	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	if (ret)
		printk("ib_req_notify_cq failed\n");
	else
		printk("ib_req_notify_cq success\n");
}

static void dtr_tx_control_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct drbd_rdma_stream *rdma_stream = ctx;
	int ret;

	printk("RDMA (control): got tx cq event. state %d\n", rdma_stream->state);

	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	if (ret)
		printk("ib_req_notify_cq failed\n");
	else
		printk("ib_req_notify_cq success\n");
}

static void dtr_tx_data_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct drbd_rdma_stream *rdma_stream = ctx;
	int ret;

	printk("RDMA (data): got tx cq event. state %d\n", rdma_stream->state);

	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	if (ret)
		printk("ib_req_notify_cq failed\n");
	else
		printk("ib_req_notify_cq success\n");
}

static int dtr_create_qp(struct drbd_rdma_stream *rdma_stream)
{
	struct ib_qp_init_attr init_attr;
	int err;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = RDMA_MAX_TX;
	init_attr.cap.max_recv_wr = RDMA_MAX_RX;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = rdma_stream->send_cq;
	init_attr.recv_cq = rdma_stream->recv_cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	err = rdma_create_qp(rdma_stream->cm_id, rdma_stream->pd, &init_attr);
	if (err) {
		printk("RDMA: rdma_create_qp failed: %d\n", err);
		return err;
	}

	rdma_stream->qp = rdma_stream->cm_id->qp;
	printk("RDMA: created qp %p\n", rdma_stream->qp);
	return 0;
}

static int dtr_post_rx_desc(struct drbd_rdma_stream *rdma_stream,
		struct drbd_rdma_rx_desc *rx_desc)
{
	struct ib_recv_wr recv_wr, *recv_wr_failed;
	int err;

	recv_wr.next       = NULL;
	recv_wr.wr_id      = (unsigned long)rx_desc;
	recv_wr.sg_list    = &rx_desc->sge;
	recv_wr.num_sge    = 1;

	ib_dma_sync_single_for_device(rdma_stream->cm_id->device,
			rx_desc->dma_addr, RDMA_PAGE_SIZE, DMA_FROM_DEVICE);

	rdma_stream->post_recv_count++;
	err = ib_post_recv(rdma_stream->qp, &recv_wr, &recv_wr_failed);
	if (err) {
		printk("RDMA: ib_post_recv error %d\n", err);
		rdma_stream->post_recv_count--;
		return err;
	}

#if 0
	printk("RDMA: Posted rx_desc: lkey=%x, addr=%llx, length=%d\n",
		 rx_desc->sge.lkey, rx_desc->sge.addr, rx_desc->sge.length);
#endif

	return 0;
}

/* RCK: for the first hack it is ok, but if we change the size of data in rx_desc, we
 * have to include "enum drbd_stream" as param */
static int dtr_create_and_post_rx_desc(struct drbd_rdma_stream *rdma_stream)
{
	struct drbd_rdma_rx_desc *rx_desc;
	struct ib_device *device = rdma_stream->cm_id->device;

	rx_desc = kzalloc(sizeof(*rx_desc), GFP_KERNEL);

	rx_desc->dma_addr = ib_dma_map_single(device, (void *)rx_desc->data,
			RDMA_PAGE_SIZE, DMA_FROM_DEVICE);
	rx_desc->sge.lkey   = rdma_stream->dma_mr->lkey;
	rx_desc->sge.addr   = rx_desc->dma_addr;
	rx_desc->sge.length = RDMA_PAGE_SIZE;

#if 0
	printk("RDMA: Created rx_desc: lkey=%x, addr=%llx, length=%d\n",
		 rx_desc->sge.lkey, rx_desc->sge.addr, rx_desc->sge.length);
#endif

	return dtr_post_rx_desc(rdma_stream, rx_desc);
}

/* RCK: we use stream to differentiate between rdma send and write:
 * control stream: rdma send
 * data stream: rdma write
 * data should be/has to be an rdma write
 * CURRENTLY only SEND */
static int dtr_post_tx_desc(struct drbd_rdma_stream *rdma_stream,
		struct drbd_rdma_tx_desc *tx_desc, enum drbd_stream stream)
{
	struct ib_device *device = rdma_stream->cm_id->device;
	struct ib_send_wr send_wr, *send_wr_failed;
	int err;
	printk("in dtr_post_tx_desc\n");

	send_wr.next = NULL;
	send_wr.wr_id = (unsigned long)tx_desc;
	send_wr.sg_list = &tx_desc->sge;
	send_wr.num_sge = 1;
	send_wr.opcode = IB_WR_SEND;
	send_wr.send_flags = IB_SEND_SIGNALED;

	ib_dma_sync_single_for_device(device, tx_desc->sge.addr,
			tx_desc->sge.length, DMA_TO_DEVICE);
	atomic_inc(&rdma_stream->post_send_count);
	err = ib_post_send(rdma_stream->qp, &send_wr, &send_wr_failed);
	if (err) {
		printk("RDMA: ib_post_send failed\n");
		atomic_dec(&rdma_stream->post_send_count);
		return err;
	}
	else {
		printk("RDMA: ib_post_send successfull!\n");
		printk("Created send_wr: lkey=%x, addr=%llx, length=%d, data[0]:%x\n",
				tx_desc->sge.lkey, tx_desc->sge.addr, tx_desc->sge.length, ((char *)tx_desc->data)[0]);
	}

	return 0;
}

static int dtr_create_and_post_tx_desc(struct drbd_rdma_stream *rdma_stream, enum drbd_stream stream, void *buf, size_t size, unsigned msg_flags)
{
	struct drbd_rdma_tx_desc *tx_desc;
	struct ib_device *device;

	/* printk("before setting device\n"); */
	if (rdma_stream->cm_id)
		printk("cm_id: %p\n", rdma_stream->cm_id);

	/* printk("before setting device\n"); */
	device = rdma_stream->cm_id->device;
	/* printk("after setting device\n"); */

	tx_desc = kzalloc(sizeof(*tx_desc), GFP_KERNEL);
	tx_desc->data = kzalloc(RDMA_PAGE_SIZE, GFP_KERNEL);
	/* printk("before memset\n"); */
	/* memset(tx_desc->data, 0x55, RDMA_PAGE_SIZE); */
	/* ((char *)(tx_desc->data))[RDMA_PAGE_SIZE -1] = 0xAA; */
#if 0
	memcpy(tx_desc->data, buf, RDMA_PAGE_SIZE);
#else
	memcpy(tx_desc->data, buf, size);
#endif

	tx_desc->sge.addr = ib_dma_map_single(device, tx_desc->data,
			RDMA_PAGE_SIZE, DMA_TO_DEVICE);
	tx_desc->sge.lkey = rdma_stream->dma_mr->lkey;
#if 0
	tx_desc->sge.length = RDMA_PAGE_SIZE;
#else
	tx_desc->sge.length = size;
#endif

	return dtr_post_tx_desc(rdma_stream, tx_desc, stream);
}

static int dtr_alloc_rdma_resources(struct drbd_rdma_stream *rdma_stream, enum drbd_stream stream)
{
	int i;
	int err;

	void (*rx_event_handler)(struct ib_cq *, void *);
	void (*tx_event_handler)(struct ib_cq *, void *);
	if (stream == DATA_STREAM) {
		rx_event_handler = dtr_rx_data_cq_event_handler;
		tx_event_handler = dtr_tx_data_cq_event_handler;
	} else {
		rx_event_handler = dtr_rx_control_cq_event_handler;
		tx_event_handler = dtr_tx_control_cq_event_handler;
	}

	printk("RDMA: here with cm_id: %p\n", rdma_stream->cm_id);

	/* alloc protection domain (PD) */
	rdma_stream->pd = ib_alloc_pd(rdma_stream->cm_id->device);
	if (IS_ERR(rdma_stream->pd)) {
		printk("RDMA: ib_alloc_pd failed\n");
		err = PTR_ERR(rdma_stream->pd);
		goto pd_failed;
	}
	printk("RDMA: created pd %p\n", rdma_stream->pd);

	/* create recv completion queue (CQ) */
	rdma_stream->recv_cq = ib_create_cq(rdma_stream->cm_id->device,
		rx_event_handler, NULL, rdma_stream, RDMA_MAX_RX, 0);
	if (IS_ERR(rdma_stream->recv_cq)) {
		printk("RDMA: ib_create_cq recv failed\n");
		err = PTR_ERR(rdma_stream->recv_cq);
		goto recv_cq_failed;
	}
	printk("RDMA: created recv cq %p\n", rdma_stream->recv_cq);

	/* create send completion queue (CQ) */
	rdma_stream->send_cq = ib_create_cq(rdma_stream->cm_id->device,
		tx_event_handler, NULL, rdma_stream, RDMA_MAX_TX, 0);
	if (IS_ERR(rdma_stream->send_cq)) {
		printk("RDMA: ib_create_cq send failed\n");
		err = PTR_ERR(rdma_stream->send_cq);
		goto send_cq_failed;
	}
	printk("RDMA: created send cq %p\n", rdma_stream->send_cq);

	/* arm CQs */
	err = ib_req_notify_cq(rdma_stream->recv_cq, IB_CQ_NEXT_COMP);
	if (err) {
		printk("RDMA: ib_req_notify_cq recv failed\n");
		goto notify_failed;
	}

	err = ib_req_notify_cq(rdma_stream->send_cq, IB_CQ_NEXT_COMP);
	if (err) {
		printk("RDMA: ib_req_notify_cq send failed\n");
		goto notify_failed;
	}

	/* create a queue pair (QP) */
	err = dtr_create_qp(rdma_stream);
	if (err) {
		printk("RDMA: create_qp error %d\n", err);
		goto createqp_failed;
	}

	/* create RDMA memory region (MR) */
	rdma_stream->dma_mr = ib_get_dma_mr(rdma_stream->pd,
			IB_ACCESS_LOCAL_WRITE |
			IB_ACCESS_REMOTE_READ |
			IB_ACCESS_REMOTE_WRITE);
	if (IS_ERR(rdma_stream->dma_mr)) {
		printk("RDMA: ib_get_dma_mr failed\n");
		err = PTR_ERR(rdma_stream->dma_mr);
		goto dma_failed;
	}

	/* fill rx desc */
	for (i = 0; i < RDMA_MAX_RX; i++) {
		err = dtr_create_and_post_rx_desc(rdma_stream);
		if (err) {
			printk("RDMA: failed posting rx desc\n");
			break;
		}
	}

	return 0;

dma_failed:
	ib_destroy_qp(rdma_stream->qp);
	rdma_stream->qp = NULL;
createqp_failed:
notify_failed:
	ib_destroy_cq(rdma_stream->send_cq);
	rdma_stream->send_cq = NULL;
send_cq_failed:
	ib_destroy_cq(rdma_stream->recv_cq);
	rdma_stream->recv_cq = NULL;
recv_cq_failed:
	ib_dealloc_pd(rdma_stream->pd);
	rdma_stream->pd = NULL;
pd_failed:
	return err;
}


static int dtr_free_stream(struct drbd_rdma_stream *rdma_stream)
{
	if (rdma_stream->dma_mr)
		ib_dereg_mr(rdma_stream->dma_mr);
	if (rdma_stream->qp)
		ib_destroy_qp(rdma_stream->qp);
	if (rdma_stream->send_cq)
		ib_destroy_cq(rdma_stream->send_cq);
	if (rdma_stream->recv_cq)
		ib_destroy_cq(rdma_stream->recv_cq);
	if (rdma_stream->pd)
		ib_dealloc_pd(rdma_stream->pd);
	if (rdma_stream->cm_id)
		rdma_destroy_id(rdma_stream->cm_id);

	kfree(rdma_stream);
	rdma_stream = NULL;
#if 0
	rdma_stream->dma_mr = NULL;
	rdma_stream->qp = NULL;
	rdma_stream->send_cq = NULL;
	rdma_stream->recv_cq = NULL;
	rdma_stream->pd = NULL;
#endif

	return 0;
}

static int dtr_free_resources(struct drbd_rdma_transport *rdma_transport)
{
	int err;

	err = dtr_free_stream(rdma_transport->stream[DATA_STREAM]);
	err |= dtr_free_stream(rdma_transport->stream[CONTROL_STREAM]);
	if (err)
		return -1;

	return 0;
}

static int dtr_connect_stream(struct drbd_rdma_stream *rdma_stream, struct sockaddr_in *peer_addr, enum drbd_stream stream)
{
	struct rdma_conn_param conn_param;
	struct sockaddr_in *peer_addr_in;
	int err;

	/* RCK: fix up this sockaddr cast mess/ipv6 hocus pocus */
	peer_addr_in = (struct sockaddr_in *)peer_addr;
	printk("RDMA: entering connect for %s\n", (stream == DATA_STREAM ? "DATA_STREAM" : "CONTROL_STREAM"));
	printk("RDMA: connecting %pI4 port %d\n",
			&peer_addr_in->sin_addr, ntohs(peer_addr_in->sin_port));

	err = dtr_create_cm_id(rdma_stream);
	if (err) {
		printk("rdma create id error %d\n", err);
		return -EINTR;
	}

	err = rdma_resolve_addr(rdma_stream->cm_id, NULL,
			(struct sockaddr *) peer_addr_in,
			2000);

	if (err) {
		printk("RDMA: rdma_resolve_addr error %d\n", err);
		return err;
	}

	wait_event_interruptible(rdma_stream->sem_state,
			rdma_stream->state >= ROUTE_RESOLVED);

	if (rdma_stream->state != ROUTE_RESOLVED) {
		printk("RDMA addr/route resolution error. state %d\n",
				rdma_stream->state);
		return err;
	}
	printk("route resolve OK\n");

	err = dtr_alloc_rdma_resources(rdma_stream, stream);
	if (err) {
		printk("RDMA: failed allocating resources %d\n", err);
		return err;
	}
	printk("RDMA: allocate resources: OK\n");

	/* Connect peer */
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	err = rdma_connect(rdma_stream->cm_id, &conn_param);
	if (err) {
		printk("RDMA: rdma_connect error %d\n", err);
		return err;
	}

	wait_event_interruptible(rdma_stream->sem_state,
			rdma_stream->state >= CONNECTED);
	if (rdma_stream->state == ERROR) {
		printk("RDMA: failed connecting. state %d\n",
				rdma_stream->state);
		return err;
	}
	printk("RDMA: rdma_connect successful\n");

	printk("RDMA: returning from connect for %s\n", (stream == DATA_STREAM ? "DATA_STREAM" : "CONTROL_STREAM"));

	return 0;
}

/* bla == Bind Listen Accept */
static int dtr_bla_stream(struct drbd_rdma_stream *rdma_stream, struct sockaddr_in *my_addr, enum drbd_stream stream)
{
	int err;
	struct rdma_conn_param conn_param;

	printk("RDMA: entering BLA for %s\n", (stream == DATA_STREAM ? "DATA_STREAM" : "CONTROL_STREAM"));
	printk("RDMA: BLA %pI4 port %d\n",
			&my_addr->sin_addr, ntohs(my_addr->sin_port));

	err = dtr_create_cm_id(rdma_stream);
	if (err) {
		printk("RDMA: rdma create id error %d\n", err);
		return -EINTR;
	}

	err = rdma_bind_addr(rdma_stream->cm_id, (struct sockaddr *) my_addr);

	if (err) {
		printk("RDMA: rdma_bind_addr error %d\n", err);
		return err;
	}

	printk("RDMA: bind success\n");

	err = rdma_listen(rdma_stream->cm_id, 3);
	if (err) {
		printk("RDMA: rdma_listen error %d\n", err);
		return err;
	}
	printk("RDMA: listen success\n");

	wait_event_interruptible(rdma_stream->sem_state,
				 rdma_stream->state >= CONNECT_REQUEST);

	if (rdma_stream->state != CONNECT_REQUEST) {
		printk("RDMA: connect request error. state %d\n",
			 rdma_stream->state);
		return err;
	}
	printk("RDMA: connect request success\n");

#if 1
	/* RCK: Again, from the contribution. Let's see if we need it */
   /* drbd_rdma_destroy_id(rdma_conn); */
	if (rdma_stream->cm_id)
		rdma_destroy_id(rdma_stream->cm_id);
	rdma_stream->cm_id = NULL;

	rdma_stream->cm_id = rdma_stream->child_cm_id;
	rdma_stream->child_cm_id = NULL;
#endif

	err = dtr_alloc_rdma_resources(rdma_stream, stream);
	if (err) {
		printk("RDMA failed allocating resources %d\n", err);
		return err;
	}
	printk("RDMA: allocated resources\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	err = rdma_accept(rdma_stream->cm_id, &conn_param);
	if (err) {
	    printk("RDMA: rdma_accept error %d\n", err);
	    return err;
	}
	printk("RMDA: connection accepted\n");

	return 0;
}


/* RCK: this way of connect requires IBoIP, but I guess that is an assumption we can make
 * If this beast will ever work, we can think about all the other ways/possible fallbacks */
static int dtr_connect(struct drbd_transport *transport)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	struct drbd_connection *connection;
	struct sockaddr_in *peer_addr, *my_addr;
	struct drbd_rdma_stream *rdma_stream;
	int err;

	connection = transport->connection;

	/* Assume that the peer only understands protocol 80 until we know better.  */
	connection->agreed_pro_version = 80;

	peer_addr = (struct sockaddr_in *)&connection->peer_addr;
	my_addr = (struct sockaddr_in *)&connection->my_addr;

	/* RCK: maybe make that one a loop */
	rdma_stream = kzalloc(sizeof(*rdma_stream), GFP_KERNEL);
	rdma_transport->stream[CONTROL_STREAM] = rdma_stream;
	rdma_stream->recv_timeout = 10000*HZ;

	/* RCK: that is of course crazy, but just a hackaround for testing until I
	 * rewrite the connection logic, rdma_server is a module param: */

	if (rdma_server)
		err = dtr_bla_stream(rdma_stream, my_addr, CONTROL_STREAM);
	else
		err = dtr_connect_stream(rdma_stream, peer_addr, CONTROL_STREAM);


	rdma_stream = kzalloc(sizeof(*rdma_stream), GFP_KERNEL);
	rdma_transport->stream[DATA_STREAM] = rdma_stream;
	/* rdma_stream->recv_timeout = ULONG_MAX; */
	rdma_stream->recv_timeout = 10000*HZ;

	if (rdma_server) {
		my_addr->sin_port += 1; /* +1 in network order, "works for me" */
		err |= dtr_bla_stream(rdma_stream, my_addr, DATA_STREAM);
	}
	else {
		peer_addr->sin_port += 1;
		schedule_timeout_uninterruptible(HZ);
		err |= dtr_connect_stream(rdma_stream, peer_addr, DATA_STREAM);
	}

	if (!err) {
		printk("RDMA: both %s streams established\n", rdma_server ? "server" : "client");
		char *buf = kzalloc(sizeof(*buf) * RDMA_PAGE_SIZE, GFP_KERNEL);
		if (rdma_server) {
			memset(buf, 0x55, RDMA_PAGE_SIZE);
			dtr_send(transport, DATA_STREAM, buf, 2, 0);
			err = dtr_recv(transport, DATA_STREAM, (void **)&buf, 1, CALLER_BUFFER);
			if (buf[0] == 0x56)
				printk("RDMA startup (server), HANDSHAKE OK\n");
			err = dtr_recv(transport, DATA_STREAM, (void **)&buf, 1, CALLER_BUFFER);
		} else {
			memset(buf, 0x56, RDMA_PAGE_SIZE);
			dtr_send(transport, DATA_STREAM, buf, 2, 0);
			err = dtr_recv(transport, DATA_STREAM, (void **)&buf, 2, CALLER_BUFFER);
			 if (buf[0] == 0x55)
				 printk("RDMA startup (client), HANDSHAKE OK\n");
			 err = dtr_recv(transport, DATA_STREAM, (void **)&buf, 1, CALLER_BUFFER);
		}
		printk("connect returns 0\n");
		return 0;
	}
	else
		printk("RDMA: connection not established :-/\n");

	if (err) /* RCK: guess it is assumed that connect() retries until successful, handle that later */
		goto out;

#if 0 /* RCK: just copied from tcp_transport, guess we will need that */
	/* Assume that the peer only understands protocol 80 until we know better.  */
	connection->agreed_pro_version = 80;

	waiter.waiter.connection = connection;
	waiter.socket = NULL;
	if (drbd_get_listener(&waiter.waiter, dtt_create_listener))
		return -EAGAIN;
#endif

	printk("connect returns 0\n");
	return 0;

out:
	dtr_free_resources(rdma_transport);
	return -EINTR;
}

static void dtr_set_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream, long timeout)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	rdma_transport->stream[stream]->recv_timeout = timeout;
}

static long dtr_get_rcvtimeo(struct drbd_transport *transport, enum drbd_stream stream)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	return rdma_transport->stream[stream]->recv_timeout;
}

static bool dtr_stream_ok(struct drbd_transport *transport, enum drbd_stream stream)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(transport, struct drbd_rdma_transport, transport);

	/* RCK: Not sure if it is a valid assumption that the stream is OK as long
	 * as the CM knows about it, but for now my best guess */
	return rdma_transport->stream[stream] && rdma_transport->stream[stream]->cm_id;
}

static int dtr_send_page(struct drbd_transport *transport, enum drbd_stream stream,
			 struct page *page, int offset, size_t size, unsigned msg_flags)
{
	struct drbd_rdma_transport *rdma_transport =
		container_of(peer_device->connection->transport, struct drbd_rdma_transport, transport);

	struct drbd_rdma_stream *rdma_stream = rdma_transport->stream[DATA_STREAM];

	/* mm_segment_t oldfs = get_fs(); */
	/* set_fs(KERNEL_DS); */

	dtr_create_and_post_tx_desc(rdma_stream, DATA_STREAM, page, size, msg_flags);

	/* set_fs(oldfs); */


	peer_device->send_cnt += size >> 9;

	return 0;
}

static bool dtr_hint(struct drbd_transport *transport, enum drbd_stream stream,
		enum drbd_tr_hints hint)
{
	switch (hint) {
	default: /* not implemented, but should not trigger error handling */
		return true;
	}
	return true;
}

static int __init dtr_init(void)
{
	return drbd_register_transport_class(&rdma_transport_class,
			DRBD_TRANSPORT_API_VERSION);
}

static void __exit dtr_cleanup(void)
{
	drbd_unregister_transport_class(&rdma_transport_class);
}

module_init(dtr_init)
module_exit(dtr_cleanup)
