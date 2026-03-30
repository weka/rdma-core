/*
 * Copyright (c) 2015-2024, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: Main component of the bnxt_re driver
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif                          /* HAVE_CONFIG_H */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <malloc.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <unistd.h>

#include "main.h"
#include "verbs.h"
#include "compat.h"

uint64_t timemark_ns(void)
{
	struct timespec tm;

	clock_gettime(CLOCK_MONOTONIC_RAW, &tm);
	return ((uint64_t)tm.tv_sec * 1000000000ULL) + tm.tv_nsec;
}

uint64_t timemark_elapsed_ns(uint64_t tm)
{
	uint64_t tm2;

	tm2 = timemark_ns();
	return tm2 - tm;
}

#define timemark_elapsed(older, newer)		((newer) - (older))

/* Inline optimizer: padding WQE constants */
#define MAX_INLINE_OPT_PAD_WQES		8
#define MAX_INLINE_OPT_PAD_SIZE		(MAX_INLINE_OPT_PAD_WQES * sizeof(struct bnxt_re_sge) * 2)
#define INLINE_OPT_MIN_SLOTS		16
#define INLINE_OPT_MAX_SLOTS		32
#define BNXT_VAR_MAX_WQE		2040

static bool bnxt_re_db_ctrl_check_push(struct bnxt_re_db_ctrl *ctrl)
{
	uint64_t tm;

	/*
	 * No activity, nothing deferred -> push
	 * No activity, stuff deferred -> DB, Push
	 * Activity -> no push
	 */
	tm = timemark_ns();
	if (timemark_elapsed(ctrl->last_tm, tm) > BNXT_RE_DB_CTRL_PUSH_TIMEOUT_NS)
		return true;
	return false;
}

#ifdef BNXT_RE_ENABLE_DEFFER_DB
/***********************
 * DB Mitigation Stuff
 */
static struct bnxt_re_db_ctrl	*bnxt_re_db_ctrl_pq;

/** Compare priorites of two db ctrls
 *
 *  higher priority is the oldest db ctrl
 *
 *  @retval = 0 if a and b are same priority, < 0 if a is lower priority of b
 *		> 0 if a is higer priority of b
 */
static inline int bnxt_re_db_ctrl_comp(struct bnxt_re_db_ctrl *a,
				       struct bnxt_re_db_ctrl *b, uint64_t tm)
{
	register uint64_t d0, d1;

	d0 = tm - a->mark_tm;
	d1 = tm - b->mark_tm;

	return d0 - d1;
}

/** Insert, in priority order, the specified db ctrl
 *
 *  Currently as a linked list.  This has peek and delete of o(1) but
 *  insertion as o(n).  max-heap/binary tree has peek of o(1) and insert/delete
 *  of o(log n).  A leftist-tree also has peek of o(1) and insert/delete of
 *  o(log n).  A leftist-tree might be better than a binary tree and similar
 *  to a max-heap so consider that.
 *
 *  Heaps will save memory too
 *  @todo redo to make faster.
 *
 *  @note must be protected by semaphore
 *
 */
static void bnxt_re_db_ctrl_pq_insert(struct bnxt_re_db_ctrl *ctrl)
{
	struct bnxt_re_db_ctrl *next, *prev;
	uint64_t	tm;

	assert(!ctrl->_next && !ctrl->_prev);
	if (!bnxt_re_db_ctrl_pq) {
		bnxt_re_db_ctrl_pq = ctrl;
		return;
	}
	tm = timemark_ns();
	next = bnxt_re_db_ctrl_pq;
	prev = NULL;
	while (next && bnxt_re_db_ctrl_comp(next, ctrl, tm) > 0) {
		assert(next != ctrl);
		prev = next;
		next = next->_next;
	}
	if (!next) {
		prev->_next = ctrl;
		ctrl->_prev = prev;
		return;
	}
	/* ctrl is higher than next */
	ctrl->_next = next;
	ctrl->_prev = next->_prev;
	next->_prev = ctrl;
	assert(ctrl->_prev == prev);
	if (!prev) {
		bnxt_re_db_ctrl_pq = ctrl;
		return;
	}
	prev->_next = ctrl;
}

static void bnxt_re_db_ctrl_pq_delete(struct bnxt_re_db_ctrl *ctrl)
{
	if (!bnxt_re_db_ctrl_pq)
		return;
	if (bnxt_re_db_ctrl_pq == ctrl) {
		bnxt_re_db_ctrl_pq = ctrl->_next;
		goto clear;
	}
	if (!ctrl->_prev && !ctrl->_next) {
		/* Not in the pq */
		return;
	}
	if (ctrl->_next)
		ctrl->_next->_prev = ctrl->_prev;

	ctrl->_prev->_next = ctrl->_next;
clear:
	ctrl->_next = NULL;
	ctrl->_prev = NULL;
}

static void bnxt_re_db_ctrl_init(struct bnxt_re_db_ctrl *ctrl, void (*ring_db)(void *))
{
	memset(ctrl, 0, sizeof(struct bnxt_re_db_ctrl));
	ctrl->ring_db = ring_db;
}

static bool bnxt_re_db_ctrl_check(struct bnxt_re_db_ctrl *ctrl);

/** Look for any DB's that are timedout */
static void bnxt_re_db_ctrl_poll(void)
{
	while (bnxt_re_db_ctrl_pq && bnxt_re_db_ctrl_check(bnxt_re_db_ctrl_pq))
		continue;
}

void bnxt_re_db_ctrl_destroy(struct bnxt_re_db_ctrl *ctrl)
{
	bnxt_re_db_ctrl_pq_delete(ctrl);
	ctrl->force = true;
	if (ctrl->opaque && ctrl->ring_db)
		bnxt_re_db_ctrl_check(ctrl);
}

static inline bool bnxt_re_db_ctrl_ready(struct bnxt_re_db_ctrl *ctrl,
					 uint64_t tm)
{
	return (ctrl->force) ||
		(ctrl->defered && (ctrl->defered >= BNXT_RE_DB_CTRL_DEFER ||
			timemark_elapsed(ctrl->mark_tm, tm) >= BNXT_RE_DB_CTRL_TIMEOUT_NS));
}

/** Check if there are deferred DBs and time to ring.
 *
 *  If time to ring, ring doorbell
 *
 *  @param ctrl		DB mitgrator instance
 *  @param tm		Recent timemark to check for aging.  Passed because some
 *				callers alrady have this info and getting it is somewhat
 *				expensive
 *  @retval True if DB rung
 */
static bool bnxt_re_db_ctrl_check(struct bnxt_re_db_ctrl *ctrl)
{
	if (bnxt_re_db_ctrl_ready(ctrl, timemark_ns())) {
		/* ring DB */
		ctrl->mark_tm = 0;
		ctrl->defered = 0;
		ctrl->ring_db(ctrl->opaque);
		ctrl->force = false;
		bnxt_re_db_ctrl_pq_delete(ctrl);
		return true;
	}
	return false;
}

/** There is a DB request.  Record that and do DB if ready
 *
 *  @retval True if DB rung
 *  @todo remove return value...not needed
 */
static void bnxt_re_db_ctrl_update(struct bnxt_re_db_ctrl *ctrl, void *db_obj)
{
	if (!ctrl->defered) {
		ctrl->mark_tm = timemark_ns();
		bnxt_re_db_ctrl_pq_insert(ctrl);
	}
	ctrl->defered++;
	ctrl->opaque = db_obj;
	/* because of db_defers cntr return bnxt_re_db_ctrl_check(ctrl) */
	bnxt_re_db_ctrl_check(ctrl);
}
#endif

static int ibv_to_bnxt_re_wr_opcd[12] = {
	BNXT_RE_WR_OPCD_RDMA_WRITE,
	BNXT_RE_WR_OPCD_RDMA_WRITE_IMM,
	BNXT_RE_WR_OPCD_SEND,
	BNXT_RE_WR_OPCD_SEND_IMM,
	BNXT_RE_WR_OPCD_RDMA_READ,
	BNXT_RE_WR_OPCD_ATOMIC_CS,
	BNXT_RE_WR_OPCD_ATOMIC_FA,
#ifdef HAVE_LOCAL_INV
	BNXT_RE_WR_OPCD_LOC_INVAL,
#else
	BNXT_RE_WR_OPCD_INVAL,
#endif
#ifdef HAVE_WR_BIND_MW
	BNXT_RE_WR_OPCD_BIND,
#else
	BNXT_RE_WR_OPCD_INVAL,
#endif
#ifdef HAVE_SEND_WITH_INV
	BNXT_RE_WR_OPCD_SEND_INVAL,
#else
	BNXT_RE_WR_OPCD_INVAL,
#endif
	BNXT_RE_WR_OPCD_INVAL,
	BNXT_RE_WR_OPCD_INVAL
};

/*
 * IBV WR opcode to BNXT_RE V3 opcode
 */
static int ibv_to_bnxt_re_wr_opcd_v3[12] = {
	BNXT_RE_WR_OPCD_RDMA_WRITE_V3,
	BNXT_RE_WR_OPCD_RDMA_WRITE_IMM_V3,
	BNXT_RE_WR_OPCD_SEND_V3,
	BNXT_RE_WR_OPCD_SEND_IMM_V3,
	BNXT_RE_WR_OPCD_RDMA_READ_V3,
	BNXT_RE_WR_OPCD_ATOMIC_CS_V3,
	BNXT_RE_WR_OPCD_ATOMIC_FA_V3,
#ifdef HAVE_LOCAL_INV
	BNXT_RE_WR_OPCD_LOC_INVAL_V3,
#else
	BNXT_RE_WR_OPCD_INVAL,
#endif
#ifdef HAVE_WR_BIND_MW
	BNXT_RE_WR_OPCD_BIND_V3,
#else
	BNXT_RE_WR_OPCD_INVAL,
#endif
#ifdef HAVE_SEND_WITH_INV
	BNXT_RE_WR_OPCD_SEND_INVAL_V3,
#else
	BNXT_RE_WR_OPCD_INVAL,
#endif
	BNXT_RE_WR_OPCD_INVAL,
#ifdef HAVE_DRIVER1
	BNXT_RE_WR_OPCD_CH_UDPSRCPORT_V3
#else
	BNXT_RE_WR_OPCD_INVAL
#endif
};

static int ibv_wr_to_wc_opcd[12] = {
	IBV_WC_RDMA_WRITE,
	IBV_WC_RDMA_WRITE,
	IBV_WC_SEND,
	IBV_WC_SEND,
	IBV_WC_RDMA_READ,
	IBV_WC_COMP_SWAP,
	IBV_WC_FETCH_ADD,
#ifdef HAVE_LOCAL_INV
	IBV_WC_LOCAL_INV,
#else
	0xFF,
#endif
#ifdef HAVE_WR_BIND_MW
	IBV_WC_BIND_MW,
#else
	0xFF,
#endif
#ifdef HAVE_SEND_WITH_INV
	IBV_WC_SEND,
#else
	0xFF,
#endif
	0xFF,
#ifdef HAVE_DRIVER1
	IBV_WC_DRIVER1
#else
	0xFF
#endif
};

static int bnxt_re_req_to_ibv_status [12] = {
	IBV_WC_SUCCESS,
	IBV_WC_BAD_RESP_ERR,
	IBV_WC_LOC_LEN_ERR,
	IBV_WC_LOC_QP_OP_ERR,
	IBV_WC_LOC_PROT_ERR,
	IBV_WC_MW_BIND_ERR,
	IBV_WC_REM_INV_REQ_ERR,
	IBV_WC_REM_ACCESS_ERR,
	IBV_WC_REM_OP_ERR,
	IBV_WC_RNR_RETRY_EXC_ERR,
	IBV_WC_RETRY_EXC_ERR,
	IBV_WC_WR_FLUSH_ERR
};

static int bnxt_re_req_to_ibv_status_v3[16] = {
	IBV_WC_SUCCESS, /* BNXT_RE_REQ_ST_OK_V3 */
	IBV_WC_BAD_RESP_ERR, /* BNXT_RE_REQ_ST_BAD_RESP_V3 */
	IBV_WC_LOC_LEN_ERR, /* BNXT_RE_REQ_ST_LOC_LEN_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_LOC_QP_OP_ERR, /* BNXT_RE_REQ_ST_LOC_QP_OP_V3 */
	IBV_WC_LOC_PROT_ERR, /* BNXT_RE_REQ_ST_PROT_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_MW_BIND_ERR, /* BNXT_RE_REQ_ST_MEM_OP_V3 */
	IBV_WC_REM_INV_REQ_ERR, /* BNXT_RE_REQ_ST_REM_INVAL_V3 */
	IBV_WC_REM_ACCESS_ERR, /* BNXT_RE_REQ_ST_REM_ACC_V3 */
	IBV_WC_REM_OP_ERR, /* BNXT_RE_REQ_ST_REM_OP_V3 */
	IBV_WC_RNR_RETRY_EXC_ERR, /* BNXT_RE_REQ_ST_RNR_NAK_XCED_V3 */
	IBV_WC_RETRY_EXC_ERR, /* BNXT_RE_REQ_ST_TRNSP_XCED_V3 */
	IBV_WC_WR_FLUSH_ERR, /* BNXT_RE_REQ_ST_WR_FLUSH_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_FATAL_ERR, /* BNXT_RE_REQ_ST_OVERFLOW_V3 */
};

static int bnxt_re_res_to_ibv_status [9] = {
	IBV_WC_SUCCESS,
	IBV_WC_LOC_ACCESS_ERR,
	IBV_WC_LOC_LEN_ERR,
	IBV_WC_LOC_PROT_ERR,
	IBV_WC_LOC_QP_OP_ERR,
	IBV_WC_MW_BIND_ERR,
	IBV_WC_REM_INV_REQ_ERR,
	IBV_WC_WR_FLUSH_ERR,
	IBV_WC_FATAL_ERR
};

static int bnxt_re_res_to_ibv_status_v3[16] = {
	IBV_WC_SUCCESS, /* BNXT_RE_RSP_ST_OK_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_LOC_LEN_ERR, /* BNXT_RE_RSP_ST_LOC_LEN_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_LOC_QP_OP_ERR, /* BNXT_RE_RSP_ST_LOC_QP_OP_V3 */
	IBV_WC_LOC_PROT_ERR, /* BNXT_RE_RSP_ST_LOC_PROT_V3 */
	IBV_WC_LOC_ACCESS_ERR, /* BNXT_RE_RSP_ST_LOC_ACC_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_REM_INV_REQ_ERR, /* BNXT_RE_RSP_ST_REM_INVAL_V3 */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_GENERAL_ERR, /* NONE */
	IBV_WC_WR_FLUSH_ERR, /* BNXT_RE_RSP_ST_WR_FLUSH_V3 */
	IBV_WC_FATAL_ERR, /* BNXT_RE_RSP_ST_HW_FLUSH_V3 */
	IBV_WC_FATAL_ERR /* BNXT_RE_RSP_ST_QVERFLOW_V3 */
};

static int bnxt_re_poll_one(struct bnxt_re_cq *cq, int nwc, struct ibv_wc *wc,
			    uint32_t *resize);

int bnxt_single_threaded;
#ifdef VERBS_ONLY_QUERY_DEVICE_EX_DEFINED
int bnxt_re_query_device_ex(struct ibv_context *ibvctx,
			    const struct ibv_query_device_ex_input *input,
			    struct ibv_device_attr_ex *attr, size_t attr_size)
{
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	struct bnxt_re_query_device_ex_resp resp = {};
	size_t resp_size = sizeof(resp);
	uint8_t fw_ver[8];
	int rc;

	rc = ibv_cmd_query_device_any(ibvctx, input, attr,
				      attr_size, &resp.resp, &resp_size);
	if (rc)
		return rc;
	memcpy(fw_ver, &resp.resp.base.fw_ver, sizeof(resp.resp.base.fw_ver));
	snprintf(attr->orig_attr.fw_ver, 64, "%d.%d.%d.%d", fw_ver[0],
		 fw_ver[1], fw_ver[2], fw_ver[3]);

	/* capture extended attr here */
	if (attr_size >=
	    offsetofend(struct ibv_device_attr_ex, packet_pacing_caps)) {
		attr->packet_pacing_caps.qp_rate_limit_min =
			resp.packet_pacing_caps.qp_rate_limit_min;
		attr->packet_pacing_caps.qp_rate_limit_max =
			resp.packet_pacing_caps.qp_rate_limit_max;
		attr->packet_pacing_caps.supported_qpts =
			resp.packet_pacing_caps.supported_qpts;
	}
	return rc;
}
#else
int bnxt_re_query_device(struct ibv_context *ibvctx,
			 struct ibv_device_attr *dev_attr)
{
	struct ibv_query_device cmd = {};
	uint8_t fw_ver[8];
	int status;

	memset(dev_attr, 0, sizeof(struct ibv_device_attr));
	status = ibv_cmd_query_device(ibvctx, dev_attr, (uint64_t *)&fw_ver,
				      &cmd, sizeof(cmd));
	snprintf(dev_attr->fw_ver, 64, "%d.%d.%d.%d",
		 fw_ver[0], fw_ver[1], fw_ver[2], fw_ver[3]);

	return status;
}
#endif

int bnxt_re_query_device_compat(struct ibv_context *ibvctx,
				struct ibv_device_attr *dev_attr)

{
#ifdef VERBS_ONLY_QUERY_DEVICE_EX_DEFINED
	struct ibv_query_device_ex_input input = {};
	struct ibv_device_attr_ex attr_ex = {};
#endif
	int rc = 0;

#ifdef VERBS_ONLY_QUERY_DEVICE_EX_DEFINED
	rc =  bnxt_re_query_device_ex(ibvctx,
				      &input,
				      &attr_ex,
				      sizeof(struct ibv_device_attr));
	if (!rc)
		memcpy(dev_attr, &attr_ex.orig_attr,
		       sizeof(struct ibv_device_attr));
#else
	rc = bnxt_re_query_device(ibvctx, dev_attr);

#endif
	return rc;
}

int bnxt_re_query_port(struct ibv_context *ibvctx, uint8_t port,
		       struct ibv_port_attr *port_attr)
{
	struct ibv_query_port cmd = {};

	return ibv_cmd_query_port(ibvctx, port, port_attr, &cmd, sizeof(cmd));
}


/* TODO: change this hardcode */
#define HDBR_PG_SHFT 12
static void bnxt_re_hdbr_unmap_dbc(void *ptr)
{
	uint64_t pg_size = 1ULL << HDBR_PG_SHFT;
	uint64_t off_mask = pg_size - 1;

	if (!ptr)
		return;
	ptr = (void *)((uint64_t)ptr & ~off_mask);
	munmap(ptr, pg_size);
}

static __u64 *bnxt_re_hdbr_map_dbc(struct ibv_context *ibvctx, uint64_t kaddr)
{
	uint64_t pg_size = 1ULL << HDBR_PG_SHFT;
	uint64_t off_mask = pg_size - 1;
	uint64_t offset;
	void *ptr;

	if (!kaddr)
		return NULL;
	offset = kaddr & off_mask;
	kaddr &= ~off_mask;
	ptr = mmap(NULL, pg_size, PROT_WRITE, MAP_SHARED | MAP_LOCKED, ibvctx->cmd_fd, kaddr);
	if (ptr == MAP_FAILED) {
		fprintf(stderr, DEV "Failed map HDBR copy memory\n");
		return NULL;
	}
	return (__u64 *)((char *)ptr + offset);
}

int bnxt_re_dv_modify_qp_v1(void *qp_handle, uint32_t type, uint32_t value)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_QP,
			       BNXT_RE_METHOD_DV_MODIFY_QP,
			       3);
	struct ibv_qp *qp = qp_handle;
	int ret;

	fill_attr_in_obj(cmd, BNXT_RE_DV_MODIFY_QP_HANDLE, qp->handle);
	fill_attr_const_in(cmd, BNXT_RE_DV_MODIFY_QP_TYPE, type);
	fill_attr_in(cmd, BNXT_RE_DV_MODIFY_QP_VALUE, &value, sizeof(value));

	ret = execute_ioctl(qp->context, cmd);
	if (ret)
		fprintf(stderr, DEV "DV Modify QP v1 error %d\n", ret);
	return ret;
}

static void bnxt_re_dv_copy_qp_attr(struct ib_uverbs_qp_attr *dst,
				    struct ibv_qp_attr *src, int attr_mask)
{
	dst->qp_state           = src->qp_state;
	dst->cur_qp_state       = src->cur_qp_state;
	dst->path_mtu           = src->path_mtu;
	dst->path_mig_state     = src->path_mig_state;
	dst->qkey               = src->qkey;
	dst->rq_psn             = src->rq_psn;
	dst->sq_psn             = src->sq_psn;
	dst->dest_qp_num        = src->dest_qp_num;
	dst->qp_access_flags    = src->qp_access_flags;
	dst->max_send_wr        = src->cap.max_send_wr;
	dst->max_recv_wr        = src->cap.max_recv_wr;
	dst->max_send_sge       = src->cap.max_send_sge;
	dst->max_recv_sge       = src->cap.max_recv_sge;
	dst->max_inline_data    = src->cap.max_inline_data;
	dst->pkey_index         = src->pkey_index;
	dst->alt_pkey_index     = src->alt_pkey_index;
	dst->en_sqd_async_notify = src->en_sqd_async_notify;
	dst->sq_draining        = src->sq_draining;
	dst->max_rd_atomic      = src->max_rd_atomic;
	dst->max_dest_rd_atomic = src->max_dest_rd_atomic;
	dst->min_rnr_timer      = src->min_rnr_timer;
	dst->port_num           = src->port_num;
	dst->timeout            = src->timeout;
	dst->retry_cnt          = src->retry_cnt;
	dst->rnr_retry          = src->rnr_retry;
	dst->alt_port_num       = src->alt_port_num;
	dst->alt_timeout        = src->alt_timeout;

	dst->qp_attr_mask = attr_mask;

	dst->ah_attr.sl = src->ah_attr.sl;
	dst->ah_attr.src_path_bits = src->ah_attr.src_path_bits;
	dst->ah_attr.port_num = src->ah_attr.port_num;
	dst->ah_attr.dlid = src->ah_attr.dlid;
	dst->ah_attr.is_global  = src->ah_attr.is_global;
	memcpy(&dst->ah_attr.grh.dgid, &src->ah_attr.grh.dgid, 16);
	dst->ah_attr.grh.sgid_index = src->ah_attr.grh.sgid_index;
	dst->ah_attr.grh.hop_limit = src->ah_attr.grh.hop_limit;
	dst->ah_attr.grh.traffic_class = src->ah_attr.grh.traffic_class;
	dst->ah_attr.grh.flow_label = src->ah_attr.grh.flow_label;
}

int bnxt_re_dv_modify_qp_v2(void *qp_handle, struct ibv_qp_attr *attr, int attr_mask)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_QP,
			       BNXT_RE_METHOD_DV_MODIFY_QP,
			       2);
	struct ibv_qp *ibvqp = qp_handle;
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct ib_uverbs_qp_attr uattr = {};
	int ret;

	bnxt_re_dv_copy_qp_attr(&uattr, attr, attr_mask);

	bnxt_trace_dv(NULL, DEV "DV Modify QP: handle: 0x%x\n", qp->qp_handle);
	fill_attr_in_obj(cmd, BNXT_RE_DV_MODIFY_QP_HANDLE, qp->qp_handle);
	fill_attr_in_ptr(cmd, BNXT_RE_DV_MODIFY_QP_REQ, &uattr);

	ret = execute_ioctl(qp->ibvqp->context, cmd);
	if (ret) {
		fprintf(stderr, DEV "DV Modify QP v2 error %d\n", ret);
		return ret;
	}

	if (attr_mask & IBV_QP_SQ_PSN)
		qp->sq_psn = attr->sq_psn;
	if (attr_mask & IBV_QP_PATH_MTU)
		qp->mtu = (0x80 << attr->path_mtu);

	return ret;
}

int bnxt_re_dv_modify_qp(void *qp_handle, struct ibv_qp_attr *attr,
			 int attr_mask, uint32_t type, uint32_t value)
{
	if (type == BNXT_RE_DV_MODIFY_QP_UDP_SPORT)
		return bnxt_re_dv_modify_qp_v1(qp_handle, type, value);
	else if (type == BNXT_RE_DV_MODIFY_QP_TYPE_NONE && attr)
		return bnxt_re_dv_modify_qp_v2(qp_handle, attr, attr_mask);
	else
		return -EINVAL;
}

int bnxt_re_dv_modify_qp_udp_sport(struct ibv_qp *ibvqp, uint16_t udp_sport)
{
	if (udp_sport)
		return bnxt_re_dv_modify_qp(ibvqp, NULL, 0,
					    BNXT_RE_DV_MODIFY_QP_UDP_SPORT, udp_sport);
	else
		return -EINVAL;
}

int bnxt_re_get_toggle_mem(struct ibv_context *ibvctx,
			   struct bnxt_re_mmap_info *minfo,
			   uint32_t *page_handle)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_GET_TOGGLE_MEM,
			       BNXT_RE_METHOD_GET_TOGGLE_MEM,
			       6);
	struct ib_uverbs_attr *handle;
	int ret;

	handle = fill_attr_out_obj(cmd, BNXT_RE_TOGGLE_MEM_HANDLE);
	fill_attr_const_in(cmd, BNXT_RE_TOGGLE_MEM_TYPE, minfo->type);
	fill_attr_in(cmd, BNXT_RE_TOGGLE_MEM_RES_ID, &minfo->res_id, sizeof(minfo->res_id));
	fill_attr_out_ptr(cmd, BNXT_RE_TOGGLE_MEM_MMAP_PAGE,  &minfo->alloc_offset);
	fill_attr_out_ptr(cmd, BNXT_RE_TOGGLE_MEM_MMAP_LENGTH, &minfo->alloc_size);
	fill_attr_out_ptr(cmd, BNXT_RE_TOGGLE_MEM_MMAP_OFFSET, &minfo->pg_offset);

	ret = execute_ioctl(ibvctx, cmd);

	if (ret)
		return ret;
	if (page_handle)
		*page_handle = read_attr_obj(BNXT_RE_TOGGLE_MEM_HANDLE, handle);
	return 0;
}

int bnxt_re_notify_drv(struct ibv_context *ibvctx)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_NOTIFY_DRV,
			       BNXT_RE_METHOD_NOTIFY_DRV,
			       0);

	return execute_ioctl(ibvctx, cmd);
}

int bnxt_re_alloc_page(struct ibv_context *ibvctx,
		       struct bnxt_re_mmap_info *minfo,
		       uint32_t *page_handle)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_ALLOC_PAGE,
			       BNXT_RE_METHOD_ALLOC_PAGE,
			       5);
	struct ib_uverbs_attr *handle;
	int ret;

	handle = fill_attr_out_obj(cmd, BNXT_RE_ALLOC_PAGE_HANDLE);
	fill_attr_const_in(cmd, BNXT_RE_ALLOC_PAGE_TYPE, minfo->type);
	fill_attr_out_ptr(cmd, BNXT_RE_ALLOC_PAGE_MMAP_OFFSET,
			  &minfo->alloc_offset);
	fill_attr_out_ptr(cmd, BNXT_RE_ALLOC_PAGE_MMAP_LENGTH, &minfo->alloc_size);
	fill_attr_out_ptr(cmd, BNXT_RE_ALLOC_PAGE_DPI, &minfo->dpi);

	ret = execute_ioctl(ibvctx, cmd);

	if (ret)
		return ret;
	if (page_handle)
		*page_handle = read_attr_obj(BNXT_RE_ALLOC_PAGE_HANDLE, handle);
	return 0;
}

static void bnxt_re_populate_pad_wqe_buffer(struct bnxt_re_pd *pd, int num_wqes)
{
	struct bnxt_re_bsqe *hdr;
	int idx;

	hdr = pd->inline_opt_pad_wqes->va_head;
	for (idx = 0; idx < num_wqes; idx++) {
		hdr->rsv_ws_fl_wt = htole32(0xc);
		hdr->key_immd = htole32(pd->inline_opt_mw_rkey);
		hdr = (void *)((char *)hdr + (2 * sizeof(*hdr)));
	}
}

static void bnxt_re_create_pad_wqe_buffer(struct bnxt_re_pd *pd)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(pd->ibvpd.context);
	struct bnxt_re_mem *mem;

	mem = bnxt_re_alloc_mem(MAX_INLINE_OPT_PAD_SIZE, cntx->rdev->pg_size, NULL);
	if (!mem) {
		fprintf(stderr, "%s: Failed to allocate padding WQE buffer!\n", __func__);
		return;
	}

	pd->inline_opt_pad_wqes = mem;
	bnxt_re_populate_pad_wqe_buffer(pd, MAX_INLINE_OPT_PAD_WQES);
}

struct ibv_pd *bnxt_re_alloc_pd(struct ibv_context *ibvctx)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_pd_resp resp = {};
	struct bnxt_re_pd_req req = {};
	struct bnxt_re_pd *pd;

	pd = calloc(1, sizeof(*pd));
	if (!pd)
		return NULL;

	if (cntx->inline_optimizer_supported) {
		/* coverity[UNUSED_VALUE:SUPPRESS] */
		req.comp_mask = BNXT_RE_COMP_MASK_REQ_PD_INLINE_OPT_MRMW;
	}

	if (ibv_cmd_alloc_pd(ibvctx, &pd->ibvpd, &req.cmd, sizeof(req),
			     &resp.resp, sizeof(resp)))
		goto out;

	pd->pdid = resp.pdid;
	pd->comp_mask = resp.comp_mask;
	pd->inline_opt_mw_rkey = (resp.comp_mask & BNXT_RE_COMP_MASK_PD_INLINE_OPT_MW_RKEY_VALID) ?
				  resp.inline_opt_mw_rkey : 0;
	pd->ibvpd.context = ibvctx;

	if (cntx->inline_optimizer_supported &&
	    (pd->comp_mask & BNXT_RE_COMP_MASK_PD_INLINE_OPT_MW_RKEY_VALID))
		bnxt_re_create_pad_wqe_buffer(pd);

	atomic_init(&pd->refcount, 1);
	return &pd->ibvpd;
out:
	free(pd);
	return NULL;
}

static struct bnxt_re_parent_domain *bnxt_re_get_parent_domain(struct ibv_pd *ibvpd)
{
	struct bnxt_re_pd *pd = to_bnxt_re_pd(ibvpd);

	if (!pd || !pd->orig_pd)
		return NULL;
	return container_of(pd, struct bnxt_re_parent_domain, pd);
}

static int bnxt_re_dealloc_parent_domain(struct bnxt_re_parent_domain *parent_domain)
{
	if (atomic_load(&parent_domain->refcount) > 1)
		return EBUSY;

	atomic_fetch_sub(&parent_domain->pd.orig_pd->refcount, 1);
	if (parent_domain->td)
		atomic_fetch_sub(&parent_domain->td->refcount, 1);

	free(parent_domain);
	return 0;
}

int bnxt_re_free_pd(struct ibv_pd *ibvpd)
{
	struct bnxt_re_parent_domain *parent_domain;
	struct bnxt_re_pd *pd;
	int status;

	pd = to_bnxt_re_pd(ibvpd);
	parent_domain = bnxt_re_get_parent_domain(ibvpd);
	if (parent_domain) {
		int ret = bnxt_re_dealloc_parent_domain(parent_domain);

		if (ret) {
			errno = ret;
			return ret;
		}
		return 0;
	}

	if (atomic_load(&pd->refcount) > 1) {
		errno = EBUSY;
		return EBUSY;
	}
	status = ibv_cmd_dealloc_pd(ibvpd);
	if (status)
		return status;

	if (pd->inline_opt_pad_wqes) {
		bnxt_re_free_mem(pd->inline_opt_pad_wqes, NULL);
		pd->inline_opt_pad_wqes = NULL;
	}

	/* DPI un-mapping will be done during uninit_ucontext */
	free(pd);
	return 0;
}

struct ibv_pd *bnxt_re_alloc_parent_domain(struct ibv_context *ibvctx,
					   struct ibv_parent_domain_init_attr *attr)
{
	struct bnxt_re_parent_domain *parent_domain;
	struct bnxt_re_pd *orig_pd;

	if (ibv_check_alloc_parent_domain(attr))
		return NULL;

	if (!check_comp_mask(attr->comp_mask,
			     IBV_PARENT_DOMAIN_INIT_ATTR_ALLOCATORS
			     | IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT)) {
		errno = EOPNOTSUPP;
		return NULL;
	}

	orig_pd = to_bnxt_re_pd(attr->pd);
	if (orig_pd->orig_pd) {
		fprintf(stderr, DEV "Nested parent domains not supported\n");
		errno = EINVAL;
		return NULL;
	}

	parent_domain = calloc(1, sizeof(*parent_domain));
	if (!parent_domain) {
		errno = ENOMEM;
		return NULL;
	}

	if (attr->td) {
		parent_domain->td = to_bnxt_re_td(attr->td);
		atomic_fetch_add(&parent_domain->td->refcount, 1);
	}

	parent_domain->pd.orig_pd = orig_pd;
	atomic_fetch_add(&orig_pd->refcount, 1);
	atomic_init(&parent_domain->refcount, 1);
	ibv_initialize_parent_domain(&parent_domain->pd.ibvpd, attr->pd);

	if (attr->comp_mask & IBV_PARENT_DOMAIN_INIT_ATTR_ALLOCATORS) {
		parent_domain->alloc = attr->alloc;
		parent_domain->free = attr->free;
	}
	if (attr->comp_mask & IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT)
		parent_domain->pd_context = attr->pd_context;

	return &parent_domain->pd.ibvpd;
}

struct ibv_td *bnxt_re_alloc_td(struct ibv_context *ibvctx,
				struct ibv_td_init_attr *init_attr)
{
	struct bnxt_re_td *td;

	if (!check_comp_mask(init_attr->comp_mask, 0)) {
		errno = EOPNOTSUPP;
		return NULL;
	}

	td = calloc(1, sizeof(*td));
	if (!td) {
		errno = ENOMEM;
		return NULL;
	}

	td->ibvtd.context = ibvctx;
	atomic_init(&td->refcount, 1);

	return &td->ibvtd;
}

int bnxt_re_dealloc_td(struct ibv_td *ibvtd)
{
	struct bnxt_re_td *td;

	td = container_of(ibvtd, struct bnxt_re_td, ibvtd);
	if (atomic_load(&td->refcount) > 1) {
		errno = EBUSY;
		return EBUSY;
	}
	free(td);
	return 0;
}

struct ibv_mr *get_ibv_mr_from_bnxt_re_mr(struct bnxt_re_mr *mr)
{
#ifndef VERBS_MR_DEFINED
	return &mr->vmr;
#else
	return &mr->vmr.ibv_mr;
#endif
}

struct ibv_mr *bnxt_re_reg_mr(struct ibv_pd *ibvpd, void *sva, size_t len,
#ifdef REG_MR_VERB_HAS_5_ARG
			      uint64_t hca_va,
#endif
			      int access)
{
	struct bnxt_re_mr_resp resp = {};
	struct ibv_reg_mr cmd = {};
	struct bnxt_re_mr *mr;
	uint64_t hw_va;
#ifdef REG_MR_VERB_HAS_5_ARG
	hw_va = hca_va;
#else
	hw_va = (uint64_t) sva;
#endif

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return NULL;

	if (ibv_cmd_reg_mr(ibvpd, sva, len, hw_va, access, &mr->vmr,
			   &cmd, sizeof(cmd), &resp.resp, sizeof(resp))) {
		free(mr);
		return NULL;
	}

	return get_ibv_mr_from_bnxt_re_mr(mr);
}

#ifdef HAVE_IBV_DMABUF
struct ibv_mr *bnxt_re_reg_dmabuf_mr(struct ibv_pd *ibvpd, uint64_t start, size_t len,
				     uint64_t iova, int fd, int access)
{
	struct ibv_command_buffer *driver = NULL;
	struct bnxt_re_mr *mr;

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return NULL;

	if (ibv_cmd_reg_dmabuf_mr_compat(ibvpd, start, len, iova, fd,
				  access, &mr->vmr, driver)) {
		free(mr);
		return NULL;
	}

	return get_ibv_mr_from_bnxt_re_mr(mr);
}
#endif

int bnxt_re_dereg_mr(VERBS_MR *ibvmr)
{
	struct bnxt_re_mr *mr = (struct bnxt_re_mr *)ibvmr;
	int status;

	status = ibv_cmd_dereg_mr(ibvmr);
	if (status)
		return status;
	free(mr);

	return 0;
}

void *bnxt_re_alloc_cqslab(struct bnxt_re_context *cntx,
			   uint32_t ncqe, uint32_t cur)
{
	struct bnxt_re_mem *mem;
	uint32_t depth, sz;

	depth = bnxt_re_init_depth(ncqe + 1, cntx->comp_mask);
	if (depth > cntx->rdev->max_cq_depth + 1)
		depth = cntx->rdev->max_cq_depth + 1;
	if (depth == cur)
		return NULL;
	sz = get_aligned((depth * cntx->rdev->cqe_size), cntx->rdev->pg_size);
	mem = bnxt_re_alloc_mem(sz, cntx->rdev->pg_size, NULL);
	if (mem)
		mem->pad = depth;
	return mem;
}

static struct ibv_cq_ex *create_cq(struct ibv_context *ibvctx,
				   struct ibv_cq_init_attr_ex *attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvctx->device);
	struct bnxt_re_cq_resp_ex resp = {};
	struct bnxt_re_cq_req_ex cmd = {};
	struct bnxt_re_cq *cq;
	int ncqe = attr->cqe;
	int ret;

	if (ncqe > dev->max_cq_depth)
		return NULL;

	cq = calloc(1, (sizeof(*cq) + sizeof(struct bnxt_re_queue)));
	if (!cq)
		return NULL;

	/* Enable deferred DB mode for CQ if the CQ is small */
	if (ncqe * 2 < dev->max_cq_depth && !cntx->cq_ignore_overrun && cntx->deferred_db_enabled) {
		cq->deffered_db_sup = true;
		ncqe = 2 * ncqe;
		attr->cqe = ncqe;
	}

	cq->cqq = (void *)((char *)cq + sizeof(*cq));
	if (!cq->cqq)
		goto mem;

	cq->mem = bnxt_re_alloc_cqslab(cntx, ncqe, 0);
	if (!cq->mem)
		goto mem;
	cq->cqq->depth = cq->mem->pad;
	cq->cqq->stride = dev->cqe_size;
	/* As an exception no need to call get_ring api we know
	 * this is the only consumer
	 */
	cq->cqq->va = cq->mem->va_head;
	if (!cq->cqq->va)
		goto fail;

	cmd.cq_va = (uint64_t)cq->cqq->va;
	cmd.cq_handle = (uint64_t)cq;
	cmd.cqprodva = (uint64_t)&cq->cqq->tail;
	cmd.cqconsva = (uint64_t)&cq->cqq->head;
	if (attr->wc_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK) {
		/* coverity[UNUSED_VALUE:SUPPRESS] */
		cmd.cq_wc = (uint64_t)&cq->current_wc;
		cmd.cq_wc_sz = sizeof(cq->current_wc);
	}
	cmd.comp_mask |= BNXT_RE_COMP_MASK_CQ_REQ_HAS_HDBR_KADDR;
	if (cntx->cq_ignore_overrun)
		cmd.comp_mask |= BNXT_RE_COMP_MASK_CQ_REQ_IGNORE_OVERRUN;

	ret = ibv_cmd_create_cq_ex_compat(ibvctx, attr,
					  &cq->verbs_cq, &cmd.cmd,
					  sizeof(cmd),
					  &resp.resp,
					  sizeof(resp),
					  0);
	if (ret)
		goto fail;

	cq->cqid = resp.cqid;
	cq->cntx = cntx;
	INIT_DBLY_LIST_HEAD(&cq->prev_cq_head);

	if (attr->wc_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK) {
		cq->ts_cq = true;
		return &cq->verbs_cq.cq_ex;
	}

	cq->phase = resp.phase;
	cq->cqq->tail = resp.tail;
	/* TBD - Why special handling for CQ HDBR ? We can remove this */
	if (resp.comp_mask & BNXT_RE_CQ_HDBR_KADDR_SUPPORT)
		cq->dbc = bnxt_re_hdbr_map_dbc(ibvctx, resp.hdbr_cq_mmap_key);
	cq->udpi = &cntx->udpi;
	cq->first_arm = true;
	cq->rand.seed = cq->cqid;
	cq->shadow_db_key = BNXT_RE_DB_KEY_INVALID;

	if (resp.comp_mask & BNXT_RE_CQ_TOGGLE_PAGE_SUPPORT) {
		struct bnxt_re_mmap_info minfo = {};
		int ret;

		minfo.type = BNXT_RE_CQ_TOGGLE_MEM;
		minfo.res_id = resp.cqid;
		ret = bnxt_re_get_toggle_mem(ibvctx, &minfo, NULL);
		if (ret)
			goto fail;
		cq->toggle_map = mmap(NULL, minfo.alloc_size, PROT_READ,
				      MAP_SHARED, ibvctx->cmd_fd,
				      minfo.alloc_offset);
		if (cq->toggle_map == MAP_FAILED)
			goto fail;

		cq->toggle_size = minfo.alloc_size;
	}

	bnxt_re_dp_spin_init(&cq->cqq->qlock, PTHREAD_PROCESS_PRIVATE, !bnxt_single_threaded);
	INIT_DBLY_LIST_HEAD(&cq->sfhead);
	INIT_DBLY_LIST_HEAD(&cq->rfhead);

	return &cq->verbs_cq.cq_ex;
fail:
	bnxt_re_free_mem(cq->mem, NULL);
mem:
	free(cq);
	return NULL;
}

struct ibv_cq *bnxt_re_create_cq(struct ibv_context *ibvctx, int ncqe,
				 struct ibv_comp_channel *channel, int vec)
{
	struct ibv_cq_init_attr_ex attr_ex = {
		.cqe = ncqe,
		.channel = channel,
		.comp_vector = vec,
		.wc_flags = 0
	};
	struct ibv_cq_ex *ibvcqx;

	ibvcqx = create_cq(ibvctx, &attr_ex);

	return ibvcqx ? ibv_cq_ex_to_cq(ibvcqx) : NULL;
}

static int bnxt_re_start_poll(struct ibv_cq_ex *ibcq,
			      struct ibv_poll_cq_attr *attr)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibv_cq_ex_to_cq(ibcq));
	struct ibv_wc wc = {0};
	int rc;

	bnxt_re_dp_spin_lock(&cq->cqq->qlock);
	rc = ibv_cmd_poll_cq(&cq->verbs_cq.cq, 1, &wc);
	if (rc <= 0) {
		errno = ENOENT;
		bnxt_re_dp_spin_unlock(&cq->cqq->qlock);
		return ENOENT;
	}

	cq->verbs_cq.cq_ex.status = wc.status;
	cq->verbs_cq.cq_ex.wr_id = wc.wr_id;

	return 0;
}

static int bnxt_re_next_poll(struct ibv_cq_ex *ibcq)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibv_cq_ex_to_cq(ibcq));
	struct ibv_wc wc = {0};
	int rc;

	rc = ibv_cmd_poll_cq(&cq->verbs_cq.cq, 1, &wc);
	if (rc <= 0) {
		errno = ENOENT;
		return ENOENT;
	}

	cq->verbs_cq.cq_ex.status = wc.status;
	cq->verbs_cq.cq_ex.wr_id = wc.wr_id;

	return 0;
}

static void bnxt_re_end_poll(struct ibv_cq_ex *ibcq)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibv_cq_ex_to_cq(ibcq));

	bnxt_re_dp_spin_unlock(&cq->cqq->qlock);
}

static uint64_t bnxt_re_read_completion_wallclock_ns(struct ibv_cq_ex *ibcq)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibv_cq_ex_to_cq(ibcq));

	return cq->current_wc.com_ns;
}

static uint32_t bnxt_re_read_src_qp(struct ibv_cq_ex *ibcq)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibv_cq_ex_to_cq(ibcq));

	return cq->current_wc.src_qp;
}

#define CREATE_CQ_SUPPORTED_WC_FLAGS	\
	IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK

static void bnxt_re_fill_cq_poll_fns(struct ibv_cq_ex *ibvcq_ex)
{
	ibvcq_ex->start_poll = bnxt_re_start_poll;
	ibvcq_ex->next_poll = bnxt_re_next_poll;
	ibvcq_ex->end_poll = bnxt_re_end_poll;
	ibvcq_ex->read_completion_wallclock_ns = bnxt_re_read_completion_wallclock_ns;
	ibvcq_ex->read_src_qp = bnxt_re_read_src_qp;
}

struct ibv_cq_ex *bnxt_re_create_cq_ex(struct ibv_context *ibvctx,
				       struct ibv_cq_init_attr_ex *cq_attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct ibv_cq_ex *ibvcq_ex;

	if (cq_attr->wc_flags & ~CREATE_CQ_SUPPORTED_WC_FLAGS) {
		fprintf(stderr, DEV "Unsupported completion flags\n");
		errno = ENOTSUP;
		return NULL;
	}

	if ((cq_attr->wc_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK) &&
	    !(cntx->comp_mask & BNXT_RE_UCNTX_CMASK_COMPLETION_TS_SUPPORTED)) {
		errno = ENOTSUP;
		return NULL;
	}

	ibvcq_ex = create_cq(ibvctx, cq_attr);
	if (!ibvcq_ex)
		return NULL;

	if (cq_attr->wc_flags)
		bnxt_re_fill_cq_poll_fns(ibvcq_ex);

	return ibvcq_ex;
}

int bnxt_re_poll_kernel_cq(struct bnxt_re_cq *cq)
{
	struct ibv_wc tmp_wc;
	int rc;

	rc = ibv_cmd_poll_cq(&cq->verbs_cq.cq, 1, &tmp_wc);
	if (unlikely(rc))
		fprintf(stderr, "ibv_cmd_poll_cq failed: %d\n", rc);
	return rc;
}

#define BNXT_RE_QUEUE_START_PHASE		0x01

/*
 * Function to complete the last steps in CQ resize. Invoke poll function
 * in the kernel driver; this serves as a signal to the driver to complete CQ
 * resize steps required. Free memory mapped for the original CQ and switch
 * over to the memory mapped for CQ with the new size. Finally Ack the Cutoff
 * CQE. This function must be called under cq->cqq.lock.
 */
void bnxt_re_resize_cq_complete(struct bnxt_re_cq *cq)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(cq->verbs_cq.cq.context);

	bnxt_re_poll_kernel_cq(cq);
	bnxt_re_free_mem(cq->mem, NULL);

	cq->mem = cq->resize_mem;
	cq->resize_mem = NULL;
	/* As an exception no need to call get_ring api we know
	 * this is the only consumer
	 */
	cq->cqq->va = cq->mem->va_head;
	/*
	 * We don't want to memcpy() the entire cqq structure below; otherwise
	 * we'd end up overwriting cq->cqq.lock that is held by the caller.
	 * So we copy the members piecemeal. cqq->head, cqq->tail implicitly
	 * set to 0 before cutoff_ack DB.
	 */
	cq->cqq->depth = cq->mem->pad;
	cq->cqq->stride = cntx->rdev->cqe_size;

	/* mark the CQ resize flag and save the old head index */
	cq->cqq->cq_resized = true;
	cq->cqq->old_head = cq->cqq->head;

	cq->cqq->head = 0;
	cq->cqq->tail = 0;
	cq->phase = BNXT_RE_QUEUE_START_PHASE;

	/* Reset epoch portion of the flags */
	cq->cqq->flags &= ~(BNXT_RE_FLAG_EPOCH_TAIL_MASK);

	bnxt_re_ring_cq_coff_ack_db(cq, BNXT_RE_QUE_TYPE_CQ_CUT_ACK);
}

int bnxt_re_resize_cq(struct ibv_cq *ibvcq, int ncqe)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvcq->context);
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvcq->context->device);
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibvcq);
	struct bnxt_re_resize_cq_req req = {};
	uint32_t exit_cnt = 20;

#ifdef RCP_USE_IB_UVERBS
	struct ib_uverbs_resize_cq_resp resp = {};
#else
	struct ibv_resize_cq_resp resp = {};
#endif
	int rc = 0;

	if (cq->ts_cq)
		return -EOPNOTSUPP;

	if (ncqe > dev->max_cq_depth)
		return -EINVAL;

	/* Check if we can be in defered DB mode with the
	 * newer size of CQE.
	 */
	cq->deffered_db_sup = false;
	if (ncqe * 2 < dev->max_cq_depth && !cntx->cq_ignore_overrun && cntx->deferred_db_enabled) {
		ncqe = 2 * ncqe;
		cq->deffered_db_sup = true;
	}

	bnxt_re_dp_spin_lock(&cq->cqq->qlock);
	cq->resize_mem = bnxt_re_alloc_cqslab(cntx, ncqe, cq->cqq->depth);
	if (unlikely(!cq->resize_mem)) {
		rc = -ENOMEM;
		goto done;
	}
	/* As an exception no need to call get_ring api we know
	 * this is the only consumer
	 */
	req.cq_va = (uint64_t)cq->resize_mem->va_head;
	rc = ibv_cmd_resize_cq(ibvcq, ncqe, &req.cmd,
			       sizeof(req), &resp, sizeof(resp));
	if (unlikely(rc)) {
		bnxt_re_free_mem(cq->resize_mem, NULL);
		goto done;
	}

	while(true) {
		struct ibv_wc tmp_wc = {0};
		uint32_t resize = 0;
		int dqed = 0;

		struct bnxt_re_work_compl *compl = NULL;
		dqed = bnxt_re_poll_one(cq, 1, &tmp_wc, &resize);
		if (resize) {
#ifdef BNXT_RE_ENABLE_DEF_ARM
			if (cq->deferred_arm) {
				bnxt_re_ring_cq_arm_db(cq, cq->deferred_arm_flags);
				cq->deferred_arm = false;
			}
#endif
			break;
		}
		if (dqed) {
			compl = calloc(1, sizeof(*compl));
			if (unlikely(!compl)) {
				fprintf(stderr, "%s: No Memory.. Continue\n", __func__);
				break;
			}
			memcpy(&compl->wc, &tmp_wc, sizeof(tmp_wc));
			bnxt_re_list_add_node(&compl->cnode, &cq->prev_cq_head);
			compl = NULL;
			memset(&tmp_wc, 0, sizeof(tmp_wc));
		} else {
			exit_cnt--;
			if (unlikely(!exit_cnt)) {
				rc = -EIO;
				break;
			} else {
				/* wait for 100 milli seconds */
				bnxt_re_sub_sec_busy_wait(100 * 1000000);
			}
		}
	}
done:
	bnxt_re_dp_spin_unlock(&cq->cqq->qlock);
	return rc;
}

static void bnxt_re_destroy_resize_cq_list(struct bnxt_re_cq *cq)
{
	struct bnxt_re_list_node *cur, *tmp;
	struct bnxt_re_work_compl *compl;

	if (bnxt_re_list_empty(&cq->prev_cq_head))
		return;

	list_for_each_node_safe(cur, tmp, &cq->prev_cq_head) {
		compl = list_node(cur, struct bnxt_re_work_compl, cnode);
		bnxt_re_list_del_node(&compl->cnode, &cq->prev_cq_head);
		free(compl);
	}

}

int bnxt_re_destroy_cq(struct ibv_cq *ibvcq)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibvcq);
	int status;

	bnxt_re_hdbr_unmap_dbc(cq->dbc);

	/* TBD - earlier munmap (cq->cq_page) was never
	 * called. Need backporting ?
	 */
	if (cq->toggle_map)
		munmap(cq->toggle_map, cq->toggle_size);

	status = ibv_cmd_destroy_cq(ibvcq);
	if (status) {
		return status;
	}
	bnxt_re_destroy_resize_cq_list(cq);
	bnxt_re_free_mem(cq->mem, NULL);
	if (cq->parent_domain)
		atomic_fetch_sub(&cq->parent_domain->refcount, 1);
	free(cq);
	return 0;
}

static inline void *bnxt_re_get_head_hwqe(struct bnxt_re_queue *que, uint32_t idx)

{
	idx += que->head;
	if (idx >= que->depth)
		idx -= que->depth;
	return (void *)(que->va + (idx << 4));
}

static void dump_cqe(struct bnxt_re_context *cntx, void *buf,
		     struct bnxt_re_wrid *swrid)
{
	__be32 *p = buf;
	int i;

	if (swrid)
		bnxt_err(cntx, "wrid 0x%lx bytes 0x%x slots 0x%x IBV_WC_OPCODE 0x%x\n",
			 swrid->wrid, swrid->bytes, swrid->slots, swrid->wc_opcd);

	bnxt_err(cntx, "Dump error cqe: \n");
	for (i = 0; i < 16; i += 4)
		bnxt_err(cntx, "%08x %08x %08x %08x\n",
			 le32toh(p[i + 1]), le32toh(p[i]),
			 le32toh(p[i + 3]), le32toh(p[i + 2]));
	if (bnxt_freeze_on_error_cqe) {
		bnxt_err(cntx, "freeze poll_cqe for debug\n");
		while (true)
			sleep(10);
	}
}

static void dump_wqe(struct bnxt_re_context *cntx, struct bnxt_re_qp *qp)
{
	struct bnxt_re_queue *sq;
	int i, j = 0;
	__be32 *buf;

	sq = qp->jsqq->hwque;

	/* TBD - add all imp counters wqe_cnt, sq_msn etc for debugging */
	bnxt_err(cntx, "Dump error wqe at slot 0x%x (%d) :\n",
		 sq->head, sq->head);
	for (i = 0; i < 16; i++) {
		/* Get the last wqe entry and dump next 16 slot entries */
		buf = bnxt_re_get_head_hwqe(sq, j++);
		bnxt_err(cntx, "%08x: %08x %08x\n", sq->head + i,
			 le32toh(buf[1]), le32toh(buf[0]));
		bnxt_err(cntx, "        : %08x %08x\n",
			 le32toh(buf[3]), le32toh(buf[2]));
	}
}

static uint8_t bnxt_re_poll_err_scqe(struct bnxt_re_qp *qp,
				     struct ibv_wc *ibvwc,
				     struct bnxt_re_req_cqe *scqe,
				     uint32_t flg_val, int *cnt)
{
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	uint8_t status, err_status = 0;
	struct bnxt_re_wrid *swrid;
	struct bnxt_re_cq *scq;
	uint32_t head;
	int arr_size;
	uint8_t pad_wqe_count;

	scq = to_bnxt_re_cq(qp->ibvqp->send_cq);

	head = qp->jsqq->last_idx;
	swrid = &qp->jsqq->swque[head];

	*cnt = 1;
	status = (flg_val >> BNXT_RE_BCQE_STATUS_SHIFT) &
		  BNXT_RE_BCQE_STATUS_MASK;

	arr_size = qp->cctx->is_hsi_v3 ?
		   ARRAY_SIZE(bnxt_re_req_to_ibv_status_v3) :
		   ARRAY_SIZE(bnxt_re_req_to_ibv_status);
	if (status > arr_size - 1) {
		fprintf(stderr, "%s Wrong req cqe status %d reported on qp_num = 0x%x\n",
			__func__, status, ibvwc->qp_num);
		/* default to IBV_WC_GENERAL_ERR*/
		err_status = IBV_WC_GENERAL_ERR;
	}

	if (err_status) {
		ibvwc->status = err_status;
		ibvwc->vendor_err = err_status;
	} else {
		ibvwc->status = qp->cctx->is_hsi_v3 ?
				bnxt_re_req_to_ibv_status_v3[status] :
				bnxt_re_req_to_ibv_status[status];
		ibvwc->vendor_err = status;
	}
	ibvwc->wc_flags = 0;
	ibvwc->wr_id = swrid->wrid;
	ibvwc->qp_num = qp->qpid;
	ibvwc->opcode = swrid->wc_opcd;
	ibvwc->byte_len = 0;

	bnxt_err(scq->cntx,
		 "%s: qp_num = 0x%x status = %d vendor error = %d prod %d cons %d\n",
		 __func__, ibvwc->qp_num, ibvwc->status, ibvwc->vendor_err,
		 qp->jsqq->hwque->tail, qp->jsqq->hwque->head);

	dump_cqe(scq->cntx, scqe, swrid);
	/* Get the last wqe entry */
	dump_wqe(scq->cntx, qp);

	pad_wqe_count = swrid->pad_wqe_count;
	bnxt_re_incr_head(sq, swrid->slots);
	if (pad_wqe_count)
		bnxt_re_incr_head(sq, (pad_wqe_count * 2));
	if (pad_wqe_count)
		bnxt_re_jqq_mod_last_batch(qp->jsqq, head, pad_wqe_count);
	else
		bnxt_re_jqq_mod_last(qp->jsqq, head);

	/*
	 * If the QP is in express mode and the original error CQE is detected,
	 * driver needs to move the QP into ERROR state
	 */
	if (qp->exp_mode && status != BNXT_RE_REQ_ST_WR_FLUSH_V3) {
		struct ibv_qp_attr attr = {};
		int attr_mask;

		attr_mask = IBV_QP_STATE;
		attr.qp_state = IBV_QPS_ERR;
		bnxt_err(scq->cntx, "%s: move QP into ERROR state\n",
			 __func__);
		bnxt_re_modify_qp(qp->ibvqp, &attr, attr_mask);
	}

	if (qp->qpst != IBV_QPS_ERR)
		qp->qpst = IBV_QPS_ERR;
	bnxt_re_list_add_node(&qp->snode, &scq->sfhead);
	qp->sq_flushed = true;

	return false;
}

static uint8_t bnxt_re_poll_success_scqe(struct bnxt_re_qp *qp,
			struct ibv_wc *ibvwc,
			struct bnxt_re_req_cqe *scqe, int *cnt)
{
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_wrid *swrid;
	uint32_t cindx, head;
	uint8_t pad_wqe_count;

	head = qp->jsqq->last_idx;
	swrid = &qp->jsqq->swque[head];

	if (qp->cctx->is_hsi_v3)
		cindx = le32toh(scqe->con_indx) % sq->depth;
	else
		cindx = le32toh(scqe->con_indx) % qp->cap.max_swr;

	if (!(swrid->sig & IBV_SEND_SIGNALED)) {
		*cnt = 0;
	} else {
		ibvwc->status = IBV_WC_SUCCESS;
		ibvwc->wc_flags = 0;
		ibvwc->qp_num = qp->qpid;
		ibvwc->wr_id = swrid->wrid;
		ibvwc->opcode = swrid->wc_opcd;
		if (ibvwc->opcode == IBV_WC_RDMA_READ ||
		    ibvwc->opcode == IBV_WC_COMP_SWAP ||
		    ibvwc->opcode == IBV_WC_FETCH_ADD)
			ibvwc->byte_len = swrid->bytes;
		*cnt = 1;
		swrid->sig = 0;
	}
	pad_wqe_count = swrid->pad_wqe_count;
	swrid->pad_wqe_count = 0;
	bnxt_re_incr_head(sq, swrid->slots);
	bnxt_re_jqq_mod_last(qp->jsqq, head);
	if (qp->cctx->is_hsi_v3) {
		if (sq->head != cindx)
			return true;
	} else {
		if (qp->jsqq->last_idx != cindx && head != cindx) {
			if (pad_wqe_count)
				bnxt_re_jqq_mod_last_batch(qp->jsqq, head, (pad_wqe_count - 1));
			return true;
		}
	}

	if (pad_wqe_count)
		bnxt_re_jqq_mod_last_batch(qp->jsqq, head, (pad_wqe_count - 1));

	return false;
}

static uint8_t bnxt_re_poll_scqe(struct bnxt_re_qp *qp, struct ibv_wc *ibvwc,
				 void *cqe, uint32_t flg_val, int *cnt)
{
	uint8_t status;

	status = (flg_val >> BNXT_RE_BCQE_STATUS_SHIFT) &
		  BNXT_RE_BCQE_STATUS_MASK;
	if (likely(status == BNXT_RE_REQ_ST_OK))
		return bnxt_re_poll_success_scqe(qp, ibvwc, cqe, cnt);
	else
		return bnxt_re_poll_err_scqe(qp, ibvwc, cqe, flg_val, cnt);
}

static void bnxt_re_release_srqe(struct bnxt_re_srq *srq, int tag)
{
	bnxt_re_dp_spin_lock(&srq->srqq->qlock);
	srq->srwrid[srq->last_idx].next_idx = tag;
	srq->last_idx = tag;
	srq->srwrid[srq->last_idx].next_idx = -1;
	bnxt_re_dp_spin_unlock(&srq->srqq->qlock);
}

static int bnxt_re_poll_err_rcqe(struct bnxt_re_qp *qp, struct ibv_wc *ibvwc,
				 struct bnxt_re_bcqe *hdr,
				 uint32_t flg_val, void *cqe)
{
	uint8_t status, cnt, err_status = 0;
	struct bnxt_re_wrid *swque = NULL;
	struct bnxt_re_queue *rq;
	struct bnxt_re_cq *rcq;
	uint32_t head = 0;
	int arr_size;

	rcq = to_bnxt_re_cq(qp->ibvqp->recv_cq);

	status = (flg_val >> BNXT_RE_BCQE_STATUS_SHIFT) &
		  BNXT_RE_BCQE_STATUS_MASK;
	/* skip h/w flush errors */
	if (status == BNXT_RE_RSP_ST_HW_FLUSH ||
	    status == BNXT_RE_RSP_ST_HW_FLUSH_V3)
		return 0;

	if (!qp->srq) {
		rq = qp->jrqq->hwque;
		head = qp->jrqq->last_idx;
		swque = &qp->jrqq->swque[head];
		ibvwc->wr_id = swque->wrid;
		cnt = swque->slots;
	} else {
		struct bnxt_re_srq *srq;
		int tag;

		srq = qp->srq;
		rq = srq->srqq;
		cnt = 1;
		tag = le32toh(hdr->qphi_rwrid) & BNXT_RE_BCQE_RWRID_MASK;
		ibvwc->wr_id = srq->srwrid[tag].wrid;
		bnxt_re_release_srqe(srq, tag);
	}

	arr_size = qp->cctx->is_hsi_v3 ?
		   ARRAY_SIZE(bnxt_re_res_to_ibv_status_v3) :
		   ARRAY_SIZE(bnxt_re_res_to_ibv_status);
	if (status > arr_size - 1) {
		fprintf(stderr, "%s Wrong resp cqe status %d reported on qp_num = 0x%x\n",
			__func__, status, ibvwc->qp_num);
		/* default to IBV_WC_GENERAL_ERR */
		err_status = IBV_WC_GENERAL_ERR;
	}
	if (err_status) {
		ibvwc->status = err_status;
		ibvwc->vendor_err = err_status;
	} else {
		ibvwc->status = qp->cctx->is_hsi_v3 ?
				bnxt_re_res_to_ibv_status_v3[status] :
				bnxt_re_res_to_ibv_status[status];
		ibvwc->vendor_err = status;
	}
	ibvwc->qp_num = qp->qpid;
	ibvwc->opcode = IBV_WC_RECV;
	ibvwc->byte_len = 0;
	ibvwc->wc_flags = 0;
	if (qp->qptyp == IBV_QPT_UD)
		ibvwc->src_qp = 0;

	if (!qp->srq)
		bnxt_re_jqq_mod_last(qp->jrqq, head);
	bnxt_re_incr_head(rq, cnt);

	if (!qp->srq) {
		bnxt_re_list_add_node(&qp->rnode, &rcq->rfhead);
		qp->rq_flushed = true;
	}

	bnxt_err(rcq->cntx, "%s: qp_num = 0x%x status = %d vendor error = %d\n",
		 __func__, ibvwc->qp_num, ibvwc->status, ibvwc->vendor_err);
	dump_cqe(rcq->cntx, cqe, swque);

	/*
	 * If QP is in express mode and the original error CQE is detected,
	 * driver needs to move the QP into ERROR state
	 */
	if (qp->exp_mode &&
	    status != BNXT_RE_RSP_ST_WR_FLUSH_V3 &&
	    status != BNXT_RE_RSP_ST_HW_FLUSH_V3) {
		struct ibv_qp_attr attr = {};
		int attr_mask;

		attr_mask = IBV_QP_STATE;
		attr.qp_state = IBV_QPS_ERR;
		bnxt_err(rcq->cntx, "%s: move QP into ERROR state\n",
			 __func__);
		bnxt_re_modify_qp(qp->ibvqp, &attr, attr_mask);
	}

	return 1;
}

static void bnxt_re_fill_ud_cqe(struct ibv_wc *ibvwc,
				struct bnxt_re_bcqe *hdr, void *cqe,
				uint8_t flags, bool is_hsi_v3)
{
	uint32_t qpid = 0;

	if (is_hsi_v3) {
		struct bnxt_re_ud_cqe_v3 *ucqe_v3 = cqe;

		qpid = ucqe_v3->qphi << 16;
		qpid |= (le64toh(ucqe_v3->qplo_mac) >> BNXT_RE_UD_CQE_SRCQPLO_SHIFT) &
			BNXT_RE_UD_CQE_SRCQPLO_MASK;
	} else {
		struct bnxt_re_ud_cqe *ucqe = cqe;

		qpid = ((le32toh(hdr->qphi_rwrid) >> BNXT_RE_BCQE_SRCQP_SHIFT) &
			BNXT_RE_BCQE_SRCQP_MASK) << 16; /* higher 8 bits of 24 */
		qpid |= (le64toh(ucqe->qplo_mac) >> BNXT_RE_UD_CQE_SRCQPLO_SHIFT) &
			BNXT_RE_UD_CQE_SRCQPLO_MASK; /*lower 16 of 24 */
	}

	ibvwc->src_qp = qpid;
	ibvwc->wc_flags |= IBV_WC_GRH;
	ibvwc->sl = (flags & BNXT_RE_UD_FLAGS_IP_VER_MASK) >>
		     BNXT_RE_UD_FLAGS_IP_VER_SFT;
	/*IB-stack ABI in user do not ask for MAC to be reported. */
}

static void bnxt_re_poll_success_rcqe(struct bnxt_re_qp *qp,
				      struct ibv_wc *ibvwc,
				      struct bnxt_re_bcqe *hdr,
				      uint32_t flg_val, void *cqe)
{
	uint8_t flags, is_imm, is_rdma;
	struct bnxt_re_rc_cqe *rcqe;
	struct bnxt_re_wrid *swque;
	struct bnxt_re_queue *rq;
	uint32_t head = 0;
	uint32_t rcqe_len;
	uint8_t cnt;

	rcqe = cqe;
	if (!qp->srq) {
		rq = qp->jrqq->hwque;
		head = qp->jrqq->last_idx;
		swque = &qp->jrqq->swque[head];
		cnt = swque->slots;
		ibvwc->wr_id = swque->wrid;
	} else {
		struct bnxt_re_srq *srq;
		int tag;

		srq = qp->srq;
		rq = srq->srqq;
		cnt = 1;
		tag = le32toh(hdr->qphi_rwrid) & BNXT_RE_BCQE_RWRID_MASK;
		ibvwc->wr_id = srq->srwrid[tag].wrid;
		bnxt_re_release_srqe(srq, tag);
	}

	ibvwc->status = IBV_WC_SUCCESS;
	ibvwc->qp_num = qp->qpid;
	rcqe_len = le32toh(rcqe->length);
	ibvwc->byte_len = (qp->qptyp == IBV_QPT_UD ||
			   qp->qptyp == IBV_QPT_RAW_PACKET) ?
			   rcqe_len & BNXT_RE_UD_CQE_LEN_MASK : rcqe_len;
	ibvwc->opcode = IBV_WC_RECV;
	ibvwc->wc_flags = 0;

	if (qp->qptyp == IBV_QPT_UD ||
	    qp->qptyp == IBV_QPT_RC) {
		flags = (flg_val >> BNXT_RE_BCQE_FLAGS_SHIFT) &
			 BNXT_RE_BCQE_FLAGS_MASK;
		is_imm = (flags & BNXT_RE_RC_FLAGS_IMM_MASK) >>
			  BNXT_RE_RC_FLAGS_IMM_SHIFT;
		is_rdma = (flags & BNXT_RE_RC_FLAGS_RDMA_MASK) >>
			   BNXT_RE_RC_FLAGS_RDMA_SHIFT;

		if (is_imm) {
			ibvwc->wc_flags |= IBV_WC_WITH_IMM;
			/* The HW is returning imm_data in little-endian format,
			 * swap to Big Endian as expected by application
			 */
			ibvwc->imm_data = htobe32(le32toh(rcqe->imm_key));
			if (is_rdma)
				ibvwc->opcode = IBV_WC_RECV_RDMA_WITH_IMM;
		}

		if (qp->qptyp == IBV_QPT_UD)
			bnxt_re_fill_ud_cqe(ibvwc, hdr, cqe, flags, qp->cctx->is_hsi_v3);
	}

	if (!qp->srq)
		bnxt_re_jqq_mod_last(qp->jrqq, head);
	bnxt_re_incr_head(rq, cnt);
}

static uint8_t bnxt_re_poll_rcqe(struct bnxt_re_qp *qp, struct ibv_wc *ibvwc,
				 void *cqe, uint32_t flg_val, int *cnt)
{
	struct bnxt_re_bcqe *hdr;
	uint8_t status, pcqe = false;

	hdr = cqe + sizeof(struct bnxt_re_rc_cqe);

	status = (flg_val >> BNXT_RE_BCQE_STATUS_SHIFT) &
		  BNXT_RE_BCQE_STATUS_MASK;
	*cnt = 1;
	if (status == BNXT_RE_RSP_ST_OK)
		bnxt_re_poll_success_rcqe(qp, ibvwc, hdr, flg_val, cqe);
	else
		*cnt = bnxt_re_poll_err_rcqe(qp, ibvwc, hdr, flg_val, cqe);

	return pcqe;
}

static void bnxt_re_qp_move_flush_err(struct bnxt_re_cq *cq, struct bnxt_re_qp *qp)
{
	struct bnxt_re_cq *scq, *rcq;

	scq = to_bnxt_re_cq(qp->ibvqp->send_cq);
	rcq = to_bnxt_re_cq(qp->ibvqp->recv_cq);

	if (qp->qpst != IBV_QPS_ERR)
		qp->qpst = IBV_QPS_ERR;
	if (!qp->srq && rcq == cq) {
		bnxt_re_list_add_node(&qp->rnode, &rcq->rfhead);
		qp->rq_flushed = true;
	}
	if (scq == cq) {
		bnxt_re_list_add_node(&qp->snode, &scq->sfhead);
		qp->sq_flushed = true;
	}
}

/* Always return false */
static uint8_t bnxt_re_poll_term_cqe(struct bnxt_re_cq *cq, struct bnxt_re_qp *qp, int *cnt)
{
	struct bnxt_re_cq *scq, *rcq, *tmp_cq = NULL;

	scq = to_bnxt_re_cq(qp->ibvqp->send_cq);
	rcq = to_bnxt_re_cq(qp->ibvqp->recv_cq);
	/* For now just add the QP to flush list without
	 * considering the index reported in the CQE.
	 * Continue reporting flush completions until the
	 * SQ and RQ are empty.
	 */
	bnxt_err(qp->cntx, "From %s qpid %d\n", __func__, qp->qpid);
	*cnt = 0;
	if (qp->qpst != IBV_QPS_RESET) {
		bnxt_re_qp_move_flush_err(cq, qp);
		if (cq == scq)
			tmp_cq = rcq;
		else
			tmp_cq = scq;
		if (tmp_cq && tmp_cq != cq) {
			bnxt_re_dp_spin_lock(&tmp_cq->cqq->qlock);
			bnxt_re_qp_move_flush_err(tmp_cq, qp);
			bnxt_re_dp_spin_unlock(&tmp_cq->cqq->qlock);
		}
	}

	return false;
}

static inline void bnxt_re_check_and_ring_cq_db(struct bnxt_re_cq *cq,
						int *hw_polled)
{
	/* Ring doorbell only if the CQ is at
	 * least half when deferred db mode is active
	 */
	if (cq->deffered_db_sup) {
		if (cq->hw_cqes < cq->cqq->depth / 2)
			return;
		*hw_polled = 0;
		cq->hw_cqes = 0;
	}
	bnxt_re_ring_cq_db(cq);
}

static int bnxt_re_poll_one(struct bnxt_re_cq *cq, int nwc, struct ibv_wc *wc,
			    uint32_t *resize)
{
	int type, cnt = 0, dqed = 0, hw_polled = 0;
	struct bnxt_re_queue *cqq = cq->cqq;
	struct bnxt_re_req_cqe *scqe;
	struct bnxt_re_ud_cqe *rcqe;
	struct bnxt_re_bcqe *hdr;
	struct bnxt_re_qp *qp;
	uint8_t pcqe = false;
	uint32_t flg_val;
	void *cqe;

#ifdef BNXT_RE_ENABLE_DEFFER_DB
	if (cq->cntx && cq->cntx->deferred_db_enabled)
		bnxt_re_db_ctrl_poll();
#endif
	while (nwc) {
		cqe = cqq->va + cqq->head * bnxt_re_get_cqe_sz();
		hdr = cqe + sizeof(struct bnxt_re_req_cqe);
		flg_val = le32toh(hdr->flg_st_typ_ph);
		if (!bnxt_re_is_cqe_valid(flg_val, cq->phase))
			break;
		bnxt_re_rm_barrier();
		type = (flg_val >> BNXT_RE_BCQE_TYPE_SHIFT) &
			BNXT_RE_BCQE_TYPE_MASK;
		switch (type) {
		case BNXT_RE_WC_TYPE_SEND:
		case BNXT_RE_WC_TYPE_SEND_V3:
			scqe = cqe;
			qp = (struct bnxt_re_qp *)
			     (uintptr_t)le64toh(scqe->qp_handle);
			if (!qp)
				break; /*stale cqe. should be rung.*/
			pcqe = bnxt_re_poll_scqe(qp, wc, cqe, flg_val, &cnt);
			break;
		case BNXT_RE_WC_TYPE_RECV_RC:
		case BNXT_RE_WC_TYPE_RECV_RC_V3:
		case BNXT_RE_WC_TYPE_RECV_UD:
		case BNXT_RE_WC_TYPE_RECV_UD_V3:
		case BNXT_RE_WC_TYPE_RECV_RAW:
		case BNXT_RE_WC_TYPE_RECV_RAW_V3:
			rcqe = cqe;
			qp = (struct bnxt_re_qp *)
			     (uintptr_t)le64toh(rcqe->qp_handle);
			if (!qp)
				break; /*stale cqe. should be rung.*/
			pcqe = bnxt_re_poll_rcqe(qp, wc, cqe, flg_val, &cnt);
			break;

		case BNXT_RE_WC_TYPE_TERM:
			scqe = cqe;
			qp = (struct bnxt_re_qp *)
			     (uintptr_t)le64toh(scqe->qp_handle);
			if (!qp)
				break;
			pcqe = bnxt_re_poll_term_cqe(cq, qp, &cnt);
			break;
		case BNXT_RE_WC_TYPE_COFF:
			/* Stop further processing and return */
			cq->resize_tog = (flg_val >> BNXT_RE_BCQE_RESIZE_TOG_SHIFT)
						& BNXT_RE_BCQE_RESIZE_TOG_MASK;
			bnxt_re_resize_cq_complete(cq);
			if (unlikely(resize))
				*resize = 1;
			return dqed;
		default:
			break;
		};

		if (pcqe)
			goto skipp_real;

		hw_polled++;
		cq->hw_cqes++;
		bnxt_re_incr_head(cq->cqq, 1);
		bnxt_re_change_cq_phase(cq);
skipp_real:
		if (cnt) {
			cnt = 0;
			dqed++;
			nwc--;
			wc++;
		}
		/* Extra check required to avoid CQ full */
		if (cq->deffered_db_sup)
			bnxt_re_check_and_ring_cq_db(cq, &hw_polled);
	}

	/* if cq_ignore_overrun is set, no need to ring CQ doorbell */
	if (likely(hw_polled) && !cq->cntx->cq_ignore_overrun)
		bnxt_re_check_and_ring_cq_db(cq, &hw_polled);

	return dqed;
}

static int bnxt_re_poll_flush_wcs(struct bnxt_re_joint_queue *jqq,
				  struct ibv_wc *ibvwc, uint32_t qpid,
				  int nwc)
{
	struct bnxt_re_queue *que;
	struct bnxt_re_wrid *wrid;
	uint32_t cnt = 0;

	que = jqq->hwque;
	while(nwc) {
		if (bnxt_re_is_que_empty(que))
			break;
		wrid = &jqq->swque[jqq->last_idx];
		ibvwc->status = IBV_WC_WR_FLUSH_ERR;
		ibvwc->vendor_err = 0;
		ibvwc->opcode = wrid->wc_opcd;
		ibvwc->wr_id = wrid->wrid;
		ibvwc->qp_num = qpid;
		ibvwc->byte_len = 0;
		ibvwc->wc_flags = 0;

		bnxt_re_jqq_mod_last(jqq, jqq->last_idx);
		bnxt_re_incr_head(que, wrid->slots);
		nwc--;
		cnt++;
		ibvwc++;
	}

	return cnt;
}

static int bnxt_re_poll_flush_wqes(struct bnxt_re_cq *cq,
				   struct bnxt_re_list_head *lhead,
				   struct ibv_wc *ibvwc,
				   uint32_t nwc)
{
	struct bnxt_re_list_node *cur, *tmp;
	struct bnxt_re_joint_queue *jqq;
	struct bnxt_re_qp *qp;
	bool sq_list = false;
	uint32_t polled = 0;

	sq_list = (lhead == &cq->sfhead) ? true : false;
	if (!bnxt_re_list_empty(lhead)) {
		list_for_each_node_safe(cur, tmp, lhead) {
			if (sq_list) {
				qp = list_node(cur, struct bnxt_re_qp, snode);
				jqq = qp->jsqq;
			} else {
				qp = list_node(cur, struct bnxt_re_qp, rnode);
				jqq = qp->jrqq;
				if (!jqq) /* Using srq no need to flush */
					goto done;
			}

			if (bnxt_re_is_que_empty(jqq->hwque))
				continue;
			polled += bnxt_re_poll_flush_wcs(jqq, ibvwc + polled,
							 qp->qpid, nwc - polled);
			if (!(nwc - polled))
				break;
		}
	}
done:
	return polled;
}

static int bnxt_re_poll_flush_lists(struct bnxt_re_cq *cq, uint32_t nwc,
				    struct ibv_wc *ibvwc)
{
	int left, polled = 0;

	polled  = bnxt_re_poll_flush_wqes(cq, &cq->sfhead, ibvwc, nwc);
	left = nwc - polled;

	if (!left)
		return polled;

	polled  += bnxt_re_poll_flush_wqes(cq, &cq->rfhead,
					   ibvwc + polled, left);
	return polled;
}

static int bnxt_re_poll_resize_cq_list(struct bnxt_re_cq *cq, uint32_t nwc,
				       struct ibv_wc *ibvwc)
{
	struct bnxt_re_list_node *cur, *tmp;
	struct bnxt_re_work_compl *compl;
	int left;

	left = nwc;
	list_for_each_node_safe(cur, tmp, &cq->prev_cq_head) {
		compl = list_node(cur, struct bnxt_re_work_compl, cnode);
		if (!left)
			break;
		memcpy(ibvwc, &compl->wc, sizeof(*ibvwc));
		ibvwc++;
		left--;
		bnxt_re_list_del_node(&compl->cnode, &cq->prev_cq_head);
		free(compl);
	}

	return nwc - left;
}


int bnxt_re_poll_cq(struct ibv_cq *ibvcq, int nwc, struct ibv_wc *wc)
{
	int dqed = 0, left = 0;
	struct bnxt_re_cq *cq;
	uint32_t resize = 0;

	cq = container_of(ibvcq, struct bnxt_re_cq, verbs_cq.cq);

	if (cq->ts_cq)
		return ibv_cmd_poll_cq(&cq->verbs_cq.cq, 1, wc);

	bnxt_re_dp_spin_lock(&cq->cqq->qlock);

	left = nwc;
	/* Check  whether we have anything to be completed from prev cq context */
	if (unlikely(!bnxt_re_list_empty(&cq->prev_cq_head))) {
		dqed = bnxt_re_poll_resize_cq_list(cq, nwc, wc);
		left = nwc - dqed;
		if (!left) {
			bnxt_re_dp_spin_unlock(&cq->cqq->qlock);
			return dqed;
		}
	}

	dqed += bnxt_re_poll_one(cq, left, wc + dqed, &resize);
#ifdef BNXT_RE_ENABLE_DEF_ARM
	if (cq->deferred_arm) {
		bnxt_re_ring_cq_arm_db(cq, cq->deferred_arm_flags);
		cq->deferred_arm = false;
	}
#endif

	/* Check if anything is there to flush. */
	left = nwc - dqed;
	if (unlikely(left && (!bnxt_re_list_empty(&cq->sfhead) ||
			      !bnxt_re_list_empty(&cq->rfhead))))
		dqed += bnxt_re_poll_flush_lists(cq, left, (wc + dqed));
	bnxt_re_dp_spin_unlock(&cq->cqq->qlock);

	return dqed;
}

void bnxt_re_cleanup_cq(struct bnxt_re_qp *qp, struct bnxt_re_cq *cq)
{
	struct bnxt_re_queue *que = cq->cqq;
	struct bnxt_re_req_cqe *scqe;
	struct bnxt_re_rc_cqe *rcqe;
	struct bnxt_re_bcqe *hdr;
	int indx, type;
	uint32_t flg_val;
	bool phase;
	void *cqe;

	if ((cq->dv_cq_flags & BNXT_DV_CQ_FLAGS_VALID) &&
	    !(cq->dv_cq_flags & BNXT_DV_CQ_FLAGS_HELPER))
		return;

	bnxt_re_dp_spin_lock(&que->qlock);
	phase = cq->phase;
	indx = que->head;
	while (indx != que->tail) {
		cqe = que->va + indx * bnxt_re_get_cqe_sz();
		hdr = cqe + sizeof(struct bnxt_re_req_cqe);
		flg_val = le32toh(hdr->flg_st_typ_ph);
		if (!bnxt_re_is_cqe_valid(flg_val, phase))
			break;
		type = (hdr->flg_st_typ_ph >> BNXT_RE_BCQE_TYPE_SHIFT) &
			BNXT_RE_BCQE_TYPE_MASK;

		if (type == BNXT_RE_WC_TYPE_COFF)
			goto loop_end;
		if (type == BNXT_RE_WC_TYPE_SEND ||
		    type == BNXT_RE_WC_TYPE_SEND_V3 ||
		    type == BNXT_RE_WC_TYPE_TERM) {
			scqe = cqe;
			if (scqe->qp_handle == (uint64_t)qp)
				scqe->qp_handle = 0ULL;
		} else {
			rcqe = cqe;
			if (rcqe->qp_handle == (uint64_t)qp)
				rcqe->qp_handle = 0ULL;
		}
loop_end:
		indx = (indx + 1) % que->depth;
		if (indx == 0)
			phase = !phase;
	}

	if (qp->sq_flushed && qp->scq == cq) {
		bnxt_re_list_del_node(&qp->snode, &cq->sfhead);
		qp->sq_flushed = false;
	}
	if (!qp->srq && qp->rq_flushed && qp->rcq == cq) {
		bnxt_re_list_del_node(&qp->rnode, &cq->rfhead);
		qp->rq_flushed = false;
	}
	bnxt_re_dp_spin_unlock(&que->qlock);
}

void bnxt_re_cq_event(struct ibv_cq *ibvcq)
{

}

int bnxt_re_arm_cq(struct ibv_cq *ibvcq, int flags)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibvcq);

	bnxt_re_dp_spin_lock(&cq->cqq->qlock);
	flags = !flags ? BNXT_RE_QUE_TYPE_CQ_ARMALL :
			 BNXT_RE_QUE_TYPE_CQ_ARMSE;

#ifdef BNXT_RE_ENABLE_DEF_ARM
	if (cq->first_arm) {
		bnxt_re_ring_cq_arm_db(cq, flags);
		cq->first_arm = false;
	}
	cq->deferred_arm = true;
	cq->deferred_arm_flags = flags;
#else
	bnxt_re_ring_cq_arm_db(cq, flags);
#endif
	bnxt_re_dp_spin_unlock(&cq->cqq->qlock);

	return 0;
}

int bnxt_re_check_qp_limits(struct bnxt_re_context *cntx,
			    struct ibv_qp_init_attr_ex *attr)
{
	struct ibv_device_attr *devattr;
	struct bnxt_re_dev *rdev;

	rdev = cntx->rdev;
	devattr = &rdev->devattr;
	if (attr->qp_type != IBV_QPT_RC &&
	    attr->qp_type != IBV_QPT_UD &&
	    attr->qp_type != IBV_QPT_RAW_PACKET)
		return EINVAL;
	if (attr->cap.max_send_sge > devattr->max_sge)
		return EINVAL;
	if (attr->cap.max_recv_sge > devattr->max_sge)
		return EINVAL;
	if (cntx->modes == BNXT_RE_WQE_MODE_VARIABLE) {
		if (cntx->db_push_mode == BNXT_RE_PUSH_MODE_PPP) {
			if (attr->cap.max_inline_data > BNXT_RE_MAX_PPP_SIZE_VAR_WQE)
				return EINVAL;
		} else {
			if (attr->cap.max_inline_data > bnxt_re_get_max_inline_size(cntx))
				return EINVAL;
		}
	} else if (attr->cap.max_inline_data > BNXT_RE_MAX_INLINE_SIZE) {
		return EINVAL;
	}

	if (cntx->modes == BNXT_RE_WQE_MODE_VARIABLE && attr->cap.max_inline_data == 0)
		attr->cap.max_inline_data = devattr->max_sge * sizeof(struct bnxt_re_sge);

	if (attr->cap.max_send_wr > devattr->max_qp_wr)
		attr->cap.max_send_wr = devattr->max_qp_wr;
	if (attr->cap.max_recv_wr > cntx->max_rq_wqes)
		attr->cap.max_recv_wr = cntx->max_rq_wqes;

	return 0;
}

static int bnxt_re_get_rq_slots(struct bnxt_re_context *cntx, uint8_t qpmode,
				uint32_t nrwr, uint32_t nsge, uint32_t *esz)
{
	struct bnxt_re_dev *rdev = cntx->rdev;
	uint32_t max_wqesz;
	uint32_t wqe_size;
	uint32_t stride;
	uint32_t slots;

	stride = sizeof(struct bnxt_re_sge);
	max_wqesz = bnxt_re_calc_rq_wqe_sz(cntx->cctx, rdev->devattr.max_sge);

	if (qpmode == BNXT_RE_WQE_MODE_STATIC && !rdev->small_rx_wqe)
		nsge = BNXT_RE_STATIC_WQE_MAX_SGE;

	wqe_size = bnxt_re_calc_rq_wqe_sz(cntx->cctx, nsge);
	if (wqe_size > max_wqesz)
		return -EINVAL;

	if (esz)
		*esz = wqe_size;

	slots = (nrwr * wqe_size) / stride;
	return slots;
}

#define BNXT_VAR_MAX_SLOT_ALIGN 256
static int bnxt_re_get_sq_slots(struct bnxt_re_context *cntx,
				uint8_t qpmode, uint32_t nswr,
				uint32_t nsge, uint32_t ils, uint32_t *esize,
				bool exp_mode)
{
	struct bnxt_re_dev *rdev = cntx->rdev;
	uint32_t max_wqesz;
	uint32_t wqe_size;
	uint32_t cal_ils;
	uint32_t stride;
	uint32_t ilsize;
	uint32_t hdr_sz;
	uint32_t slots;
	uint32_t align;

	if (qpmode == BNXT_RE_WQE_MODE_STATIC && cntx->cctx->is_hsi_v3)
		return -EINVAL;

	hdr_sz = bnxt_re_get_sqe_hdr_sz(cntx->cctx, false);
	stride = sizeof(struct bnxt_re_sge);
	align = hdr_sz;
	if (qpmode == BNXT_RE_WQE_MODE_VARIABLE)
		align = stride;
	max_wqesz = bnxt_re_calc_sq_wqe_sz(cntx->cctx, rdev->devattr.max_sge, ils, false);
	ilsize = get_aligned(ils, align);

	wqe_size = bnxt_re_calc_sq_wqe_sz(cntx->cctx, nsge, ils, false);
	if (ilsize) {
		cal_ils = hdr_sz + ilsize;
		wqe_size = MAX(cal_ils, wqe_size);
		wqe_size = get_aligned(wqe_size, align);
	}
	if (wqe_size > max_wqesz)
		return -EINVAL;

	if (qpmode == BNXT_RE_WQE_MODE_STATIC)
		wqe_size = bnxt_re_calc_sq_wqe_sz(cntx->cctx, 6, ils, false);

	if (esize)
		*esize = wqe_size;
	slots = (nswr * wqe_size) / stride;
	if (qpmode == BNXT_RE_WQE_MODE_VARIABLE && !exp_mode)
		slots = get_aligned(slots, BNXT_VAR_MAX_SLOT_ALIGN);
	return slots;
}

int bnxt_re_get_sqmem_size(struct bnxt_re_context *cntx,
			   struct ibv_qp_init_attr_ex *attr,
			   struct bnxt_re_qattr *qattr)
{
	uint32_t nsge, nswr, diff = 0;
	size_t bytes = 0;
	uint32_t psn_sz;
	uint32_t npsn;
	uint32_t ils;
	uint8_t mode;
	uint32_t esz;
	int nslots;

	mode = cntx->modes & BNXT_RE_WQE_MODE_VARIABLE;
	nsge = attr->cap.max_send_sge;
	diff = bnxt_re_get_diff(cntx->comp_mask);
	nswr = attr->cap.max_send_wr + 1 + diff;
	nswr = bnxt_re_init_depth(nswr, cntx->comp_mask);
	ils = qattr->exp_mode ? sizeof(struct bnxt_re_sge) :
		attr->cap.max_inline_data;
	nslots = bnxt_re_get_sq_slots(cntx, mode, nswr,
				      nsge, ils, &esz, qattr->exp_mode);
	if (nslots < 0)
		return nslots;
	npsn = bnxt_re_get_npsn(mode, nswr, nslots);
	if (BNXT_RE_MSN_TBL_EN(cntx))
		npsn = roundup_pow_of_two(npsn);
	psn_sz = bnxt_re_get_psne_size(cntx);

	qattr->nwr = nswr;
	qattr->slots = nslots;
	qattr->esize = esz;
	if (mode)
		qattr->sw_nwr = nslots;
	else
		qattr->sw_nwr = nswr;
	qattr->psn_sz = psn_sz;
	qattr->npsn = npsn;

	bytes = nslots * sizeof(struct bnxt_re_sge); /* ring */
	bytes += npsn * psn_sz; /* psn */
	qattr->sz_ring = get_aligned(bytes, cntx->rdev->pg_size);
	qattr->sz_shad = qattr->sw_nwr * sizeof(struct bnxt_re_wrid); /* shadow */
	return 0;
}

int bnxt_re_get_rqmem_size(struct bnxt_re_context *cntx,
			   struct ibv_qp_init_attr_ex *attr,
			   struct bnxt_re_qattr *qattr)
{
	uint32_t nrwr, nsge;
	size_t bytes = 0;
	uint32_t esz;
	int nslots;

	nsge = attr->cap.max_recv_sge;
	nrwr = attr->cap.max_recv_wr + 1;
	nrwr = bnxt_re_init_depth(nrwr, cntx->comp_mask);
	nslots = bnxt_re_get_rq_slots(cntx, cntx->modes,
				      nrwr, nsge, &esz);
	if (nslots < 0)
		return nslots;
	qattr->nwr = nrwr;
	qattr->slots = nslots;
	qattr->esize = esz;
	qattr->sw_nwr = nrwr;

	bytes = nslots * sizeof(struct bnxt_re_sge);
	qattr->sz_ring = get_aligned(bytes, cntx->rdev->pg_size);
	qattr->sz_shad = nrwr * sizeof(struct bnxt_re_wrid);
	return 0;
}

static int bnxt_re_get_qpmem_size(struct bnxt_re_context *cntx,
				  struct ibv_qp_init_attr_ex *attr,
				  struct bnxt_re_qattr *qattr)
{
	int size = 0;
	int tmp;
	int rc;

	size = sizeof(struct bnxt_re_qp);
	tmp = sizeof(struct bnxt_re_joint_queue);
	tmp += sizeof(struct bnxt_re_queue);
	size += tmp;

	rc = bnxt_re_get_sqmem_size(cntx, attr, &qattr[BNXT_RE_QATTR_SQ_INDX]);
	if (rc < 0)
		return -EINVAL;
	size += qattr[BNXT_RE_QATTR_SQ_INDX].sz_ring;
	size += qattr[BNXT_RE_QATTR_SQ_INDX].sz_shad;

	if (!attr->srq) {
		tmp = sizeof(struct bnxt_re_joint_queue);
		tmp += sizeof(struct bnxt_re_queue);
		size += tmp;
		rc = bnxt_re_get_rqmem_size(cntx, attr,
					    &qattr[BNXT_RE_QATTR_RQ_INDX]);
		if (rc < 0)
			return -EINVAL;
		size += qattr[BNXT_RE_QATTR_RQ_INDX].sz_ring;
		size += qattr[BNXT_RE_QATTR_RQ_INDX].sz_shad;
	}
	return size;
}

void *bnxt_re_alloc_qpslab(struct bnxt_re_context *cntx,
			   struct ibv_qp_init_attr_ex *attr,
			   struct bnxt_re_qattr *qattr)
{
	struct bnxt_re_parent_domain *pa_dom;
	int bytes;

	bytes = bnxt_re_get_qpmem_size(cntx, attr, qattr);
	if (bytes < 0)
		return NULL;
	pa_dom = bnxt_re_get_parent_domain(attr->pd);
	return bnxt_re_alloc_mem(bytes, cntx->rdev->pg_size, pa_dom);
}

int bnxt_re_alloc_queue_ptr(struct bnxt_re_qp *qp,
			    struct ibv_qp_init_attr_ex *attr)
{
	int rc = -ENOMEM;
	int jqsz, qsz;

	jqsz = sizeof(struct bnxt_re_joint_queue);
	qsz = sizeof(struct bnxt_re_queue);
	qp->jsqq = bnxt_re_get_obj(qp->mem, jqsz);
	if (!qp->jsqq)
		return rc;
	qp->jsqq->hwque = bnxt_re_get_obj(qp->mem, qsz);
	if (!qp->jsqq->hwque)
		goto fail;

	if (!attr->srq) {
		qp->jrqq = bnxt_re_get_obj(qp->mem, jqsz);
		if (!qp->jrqq)
			goto fail;
		qp->jrqq->hwque = bnxt_re_get_obj(qp->mem, qsz);
		if (!qp->jrqq->hwque)
			goto fail;
	}

	return 0;
fail:
	return rc;
}

static int bnxt_re_alloc_init_swque(struct bnxt_re_joint_queue *jqq,
				    struct bnxt_re_mem *mem,
				    struct bnxt_re_qattr *qattr)
{
	int indx;

	jqq->swque = bnxt_re_get_obj(mem, qattr->sz_shad);
	if (!jqq->swque)
		return -ENOMEM;
	jqq->start_idx = 0;
	jqq->last_idx = qattr->sw_nwr - 1;
	for (indx = 0; indx < qattr->sw_nwr; indx++)
		jqq->swque[indx].next_idx = indx + 1;
	jqq->swque[jqq->last_idx].next_idx = 0;
	jqq->last_idx = 0;

	return 0;
}

int bnxt_re_alloc_queues(struct bnxt_re_qp *qp,
			 struct ibv_qp_init_attr_ex *attr,
			 struct bnxt_re_qattr *qattr)
{
	struct bnxt_re_context *cntx;
	struct bnxt_re_queue *que;
	struct bnxt_re_parent_domain *pa_dom;
	uint32_t psn_size;
	uint8_t indx;
	int need_lock;
	int ret;

	cntx = qp->cntx;
	pa_dom = bnxt_re_pd_get_parent_domain(qp->re_pd);
	need_lock = !bnxt_single_threaded && (!pa_dom || !pa_dom->td);

	indx = BNXT_RE_QATTR_SQ_INDX;
	que = qp->jsqq->hwque;
	que->stride = sizeof(struct bnxt_re_sge);
	que->depth = qattr[indx].slots;
	que->diff = (bnxt_re_get_diff(cntx->comp_mask) * qattr[indx].esize) /
		     que->stride;
	que->va = bnxt_re_get_ring(qp->mem, qattr[indx].sz_ring);
	if (!que->va)
		return -ENOMEM;
	/* PSN-search memory is allocated without checking for
	 * QP-Type. Kernel driver do not map this memory if it
	 * is UD-qp. UD-qp use this memory to maintain WC-opcode.
	 * See definition of bnxt_re_fill_psns() for the use case.
	 */
	que->pad = (que->va + que->depth * que->stride);
	psn_size = bnxt_re_get_psne_size(qp->cntx);
	que->pad_stride_log2 = (uint32_t)log2((double)psn_size);

	ret = bnxt_re_alloc_init_swque(qp->jsqq, qp->mem, &qattr[indx]);
	if (ret)
		goto fail;

	qp->cap.max_swr = qattr[indx].sw_nwr;
	qp->jsqq->cntx = qp->cntx;
	qp->jsqq->swque_sz = qattr[indx].sw_nwr;
	que->dbtail = (qp->qpmode == BNXT_RE_WQE_MODE_VARIABLE) ?
		       &que->tail : &qp->jsqq->start_idx;

	/* Init and adjust MSN table size according to qp mode */
	if (!BNXT_RE_MSN_TBL_EN(qp->cntx) || BNXT_RE_IQM(qp->cntx))
		goto skip_msn;
	que->msn = 0;
	que->msn_tbl_sz = 0;
	if (qp->qpmode & BNXT_RE_WQE_MODE_VARIABLE)
		que->msn_tbl_sz = roundup_pow_of_two(qattr->slots) / 2;
	else
		que->msn_tbl_sz = roundup_pow_of_two(qattr->nwr);
skip_msn:
	bnxt_re_dp_spin_init(&que->qlock, PTHREAD_PROCESS_PRIVATE, need_lock);

	if (qp->jrqq) {
		indx = BNXT_RE_QATTR_RQ_INDX;
		que = qp->jrqq->hwque;
		que->stride = sizeof(struct bnxt_re_sge);
		que->depth = qattr[indx].slots;
		que->max_slots = qattr[indx].esize / que->stride;
		que->dbtail = &qp->jrqq->start_idx;
		que->va = bnxt_re_get_ring(qp->mem, qattr[indx].sz_ring);
		if (!que->va)
			return -ENOMEM;
		/* For RQ only bnxt_re_wri.wrid is used. */
		ret = bnxt_re_alloc_init_swque(qp->jrqq, qp->mem, &qattr[indx]);
		if (ret)
			goto fail;

		bnxt_re_dp_spin_init(&que->qlock, PTHREAD_PROCESS_PRIVATE, need_lock);
		qp->cap.max_rwr = qattr[indx].nwr;
		qp->jrqq->swque_sz = qp->cap.max_rwr;
		qp->jrqq->cntx = qp->cntx;
	}

	if (pa_dom) {
		qp->parent_domain = pa_dom;
		atomic_fetch_add(&pa_dom->refcount, 1);
	} else {
		qp->parent_domain = NULL;
	}
	return 0;
fail:
	return ret;
}

#ifdef ASYNC_EVENT_VERB_HAS_1_ARG
void bnxt_re_async_event(struct ibv_async_event *event)
#else
void bnxt_re_async_event(struct ibv_context *context,
		      struct ibv_async_event *event)
#endif
{
	struct bnxt_re_cq *scq, *rcq;
	struct ibv_qp *ibvqp;
	struct bnxt_re_qp *qp;

	switch (event->event_type) {
	case IBV_EVENT_CQ_ERR:
		break;
	case IBV_EVENT_SRQ_ERR:
	case IBV_EVENT_QP_FATAL:
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_PATH_MIG_ERR: {
		ibvqp = event->element.qp;
		qp = to_bnxt_re_qp(ibvqp);
		scq = to_bnxt_re_cq(qp->ibvqp->send_cq);
		rcq = to_bnxt_re_cq(qp->ibvqp->recv_cq);
		bnxt_re_dp_spin_lock(&scq->cqq->qlock);
		bnxt_re_qp_move_flush_err(scq, qp);
		bnxt_re_dp_spin_unlock(&scq->cqq->qlock);
		if (rcq && rcq != scq) {
			bnxt_re_dp_spin_lock(&rcq->cqq->qlock);
			bnxt_re_qp_move_flush_err(rcq,  qp);
			bnxt_re_dp_spin_unlock(&rcq->cqq->qlock);
		}
		break;
	}
	case IBV_EVENT_SQ_DRAINED:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_QP_LAST_WQE_REACHED:
	case IBV_EVENT_SRQ_LIMIT_REACHED:
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_PORT_ERR:
	default:
		break;
	}
}

static void *bnxt_re_pull_psn_buff(struct bnxt_re_queue *que, bool hw_retx)
{
	if (hw_retx)
		return (void *)(que->pad + ((que->msn) << que->pad_stride_log2));
	return (void *)(que->pad + ((*que->dbtail) << que->pad_stride_log2));
}

static void bnxt_re_fill_psns_for_msntbl(struct bnxt_re_qp *qp, uint32_t len,
					 uint32_t st_idx, uint8_t opcode)
{
	uint32_t npsn = 0, start_psn = 0, next_psn = 0;
	struct bnxt_re_msns *msns;
	uint32_t pkt_cnt = 0;

	msns = bnxt_re_pull_psn_buff(qp->jsqq->hwque, true);
	msns->start_idx_next_psn_start_psn = 0;

	if (qp->qptyp == IBV_QPT_RC) {
		start_psn = qp->sq_psn;
		pkt_cnt = (len / qp->mtu);
		if (len % qp->mtu)
			pkt_cnt++;
		/* Increment the psn even for 0 len packets
		 * e.g. for opcode rdma-write-with-imm-data
		 * with length field = 0
		 */
		if (bnxt_re_is_zero_len_pkt(len, opcode))
			pkt_cnt = 1;
		/* make it 24 bit */
		next_psn = qp->sq_psn + pkt_cnt;
		npsn = next_psn;
		qp->sq_psn = next_psn;
		msns->start_idx_next_psn_start_psn |=
			bnxt_re_update_msn_tbl(st_idx, npsn, start_psn);
#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
		dump_dbg_msn_tbl(qp, msns, len, st_idx);
#endif
		qp->jsqq->hwque->msn++;
		qp->jsqq->hwque->msn %= qp->jsqq->hwque->msn_tbl_sz;
	}
}

static void bnxt_re_fill_psns(struct bnxt_re_qp *qp, uint32_t len,
			      uint32_t st_idx, uint8_t opcode)
{
	uint32_t opc_spsn = 0, flg_npsn = 0;
	struct bnxt_re_psns_ext *psns_ext;
	uint32_t pkt_cnt = 0, nxt_psn = 0;
	struct bnxt_re_psns *psns;

	psns = bnxt_re_pull_psn_buff(qp->jsqq->hwque, false);
	psns_ext = (struct bnxt_re_psns_ext *)psns;

	if (qp->qptyp == IBV_QPT_RC) {
		opc_spsn = qp->sq_psn & BNXT_RE_PSNS_SPSN_MASK;
		pkt_cnt = (len / qp->mtu);
		if (len % qp->mtu)
			pkt_cnt++;
		/* Increment the psn even for 0 len packets
		 * e.g. for opcode rdma-write-with-imm-data
		 * with length field = 0
		 */
		if (bnxt_re_is_zero_len_pkt(len, opcode))
			pkt_cnt = 1;
		nxt_psn = ((qp->sq_psn + pkt_cnt) & BNXT_RE_PSNS_NPSN_MASK);
		flg_npsn = nxt_psn;
		qp->sq_psn = nxt_psn;
	}
	psns->opc_spsn = htole32(opc_spsn);
	psns->flg_npsn = htole32(flg_npsn);
	/* Update for Thor p5 not Thor2 */
	if (!BNXT_RE_MSN_TBL_EN(qp->cntx) && qp->cctx->chip_is_gen_p5_p7)
		psns_ext->st_slot_idx = st_idx;
}

static inline void *bnxt_re_get_hwqe_no_wrap(struct bnxt_re_queue *que,
					     uint32_t idx, unsigned int num)
{
	idx += que->tail;
	if (unlikely(idx + num >= que->depth))
		return NULL;
	return (void *)(que->va + (idx << 4));
}

#ifdef HAVE_IBV_WR_API
static inline void bnxt_re_set_wr_hdr_flags(struct bnxt_re_qp *qp,
					    unsigned int send_flags)
{
	uint32_t hdrval = 0;
	uint8_t opcd;

	if (send_flags & IBV_SEND_SIGNALED || qp->cap.sqsig)
		hdrval |= ((BNXT_RE_WR_FLAGS_SIGNALED & BNXT_RE_HDR_FLAGS_MASK)
				<< BNXT_RE_HDR_FLAGS_SHIFT);
	if (send_flags & IBV_SEND_FENCE)
		/*TODO: See when RD fence can be used. */
		hdrval |= ((BNXT_RE_WR_FLAGS_UC_FENCE & BNXT_RE_HDR_FLAGS_MASK)
				<< BNXT_RE_HDR_FLAGS_SHIFT);
	if (send_flags & IBV_SEND_SOLICITED)
		hdrval |= ((BNXT_RE_WR_FLAGS_SE & BNXT_RE_HDR_FLAGS_MASK)
				<< BNXT_RE_HDR_FLAGS_SHIFT);
	if (send_flags & IBV_SEND_INLINE)
		hdrval |= ((BNXT_RE_WR_FLAGS_INLINE & BNXT_RE_HDR_FLAGS_MASK)
				<< BNXT_RE_HDR_FLAGS_SHIFT);
	hdrval |= ((qp->wr_sq.used_slot_cnt) & BNXT_RE_HDR_WS_MASK) << BNXT_RE_HDR_WS_SHIFT;
	opcd = ibv_to_bnxt_re_wr_opcd[qp->wr_sq.cur_opcode];
	hdrval |= (opcd & BNXT_RE_HDR_WT_MASK);
	qp->wr_sq.cur_hdr->rsv_ws_fl_wt = htole32(hdrval);
}

static inline void *bnxt_re_get_wr_swqe(struct bnxt_re_joint_queue *jqq,
					uint32_t cnt)
{
	return &jqq->swque[jqq->start_idx + cnt];
}

static uint16_t bnxt_re_put_wr_inline(struct bnxt_re_queue *que, uint32_t *idx,
				      struct bnxt_re_push_buffer *pbuf, size_t num_buf,
				      const struct ibv_data_buf *buf_list, size_t *msg_len)
{
	int len, t_len, offt = 0;
	int t_cplen = 0, cplen;
	int alsize, indx, num;
	bool pull_dst = true;
	void *il_dst;
	void *il_src;

	t_len = 0;
	num = (*msg_len + MSG_LEN_ADJ_TO_BYTES) >> SLOTS_RSH_TO_NUM_WQE;
	il_dst = bnxt_re_get_hwqe_no_wrap(que, *idx, num);
	if (likely(il_dst)) {
		if (pbuf) {
			pbuf->wqe[(*idx)++] = (__u64)il_dst;
			*idx += num - 1;
		} else {
			*idx += num;
		}
		for (indx = 0; indx < num_buf; indx++) {
			len = buf_list[indx].length;
			il_src = (void *)buf_list[indx].addr;
			t_len += len;
			memcpy(il_dst, il_src, len);
			il_dst += len;
		}
		return t_len;
	}
	/* move on to queue wrap case */
	alsize = sizeof(struct bnxt_re_sge);
	for (indx = 0; indx < num_buf; indx++) {
		len = buf_list[indx].length;
		il_src = (void *)buf_list[indx].addr;
		t_len += len;
		while (len) {
			if (pull_dst) {
				pull_dst = false;
				il_dst = bnxt_re_get_hwqe(que, (*idx)++);
				if (pbuf)
					pbuf->wqe[*idx - 1] = (__u64)il_dst;
				t_cplen = 0;
				offt = 0;
			}
			cplen = MIN(len, alsize);
			cplen = MIN(cplen, (alsize - offt));
			memcpy(il_dst, il_src, cplen);
			t_cplen += cplen;
			il_src += cplen;
			il_dst += cplen;
			offt += cplen;
			len -= cplen;
			if (t_cplen == alsize)
				pull_dst = true;
		}
	}
	/* Use msg_len field to indicate whether the queue wrapped or not */
	*msg_len = 0;
	return t_len;
}

static inline void bnxt_re_update_wr_common_hdr(struct bnxt_re_qp *qp, uint8_t opcode)
{
	struct bnxt_re_queue *sq = qp->jsqq->hwque;

	qp->wr_sq.cur_hdr = bnxt_re_get_hwqe(sq, qp->wr_sq.cur_slot_cnt++);
	qp->wr_sq.cur_sqe = bnxt_re_get_hwqe(sq, qp->wr_sq.cur_slot_cnt++);
	qp->wr_sq.cur_opcode = opcode;
}

static inline void bnxt_re_update_sge(struct bnxt_re_sge *sge, uint32_t lkey,
				      uint64_t addr, uint32_t length)
{
	sge->pa = htole64(addr);
	sge->lkey = htole32(lkey);
	sge->length = htole32(length);
}

static inline void bnxt_re_update_swqe(struct ibv_qp_ex *ibvqp, struct bnxt_re_qp *qp,
				       uint32_t length)
{
	struct bnxt_re_wrid *wrid;

	wrid = bnxt_re_get_wr_swqe(qp->jsqq, qp->wr_sq.cur_wqe_cnt);
	wrid->wrid = ibvqp->wr_id;
	wrid->bytes = length;
	wrid->slots = (qp->qpmode == BNXT_RE_WQE_MODE_STATIC) ?
		STATIC_WQE_NUM_SLOTS : qp->wr_sq.used_slot_cnt;
	wrid->sig = (ibvqp->wr_flags & IBV_SEND_SIGNALED || qp->cap.sqsig) ?
		IBV_SEND_SIGNALED : 0;
	wrid->wc_opcd = ibv_wr_to_wc_opcd[qp->wr_sq.cur_opcode];
}

static void bnxt_re_send_wr_start(struct ibv_qp_ex *ibvqp)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;

	bnxt_re_dp_spin_lock(&sq->qlock);
	qp->wr_sq.cur_hdr = 0;
	qp->wr_sq.cur_sqe = 0;
	qp->wr_sq.cur_slot_cnt = 0;
	qp->wr_sq.used_slot_cnt = 0;
	qp->wr_sq.cur_wqe_cnt = 0;
	qp->wr_sq.cur_opcode = 0xff;
	qp->wr_sq.cur_push_wqe = false;
	qp->wr_sq.cur_push_size = 0;
	qp->wr_sq.cur_swq_idx = qp->jsqq->start_idx;
}

static int bnxt_re_send_wr_complete(struct ibv_qp_ex *ibvqp)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	int err = qp->wr_sq.error;
	uint32_t slots;

	if (unlikely(err))
		goto exit;
	qp->wqe_cnt += qp->wr_sq.cur_wqe_cnt;
	qp->sq_msn += qp->wr_sq.cur_wqe_cnt;
	slots = (qp->qpmode == BNXT_RE_WQE_MODE_STATIC) ?
		STATIC_WQE_NUM_SLOTS : qp->wr_sq.cur_slot_cnt;
	bnxt_re_incr_tail(sq, slots);
	bnxt_re_jqq_mod_start(qp->jsqq, qp->wr_sq.cur_swq_idx + qp->wr_sq.cur_wqe_cnt - 1, 0);
	if (!qp->wr_sq.cur_push_wqe) {
		bnxt_re_ring_sq_db(qp);
	} else {
		struct bnxt_re_push_buffer *pushb;

		pushb = (struct bnxt_re_push_buffer *)qp->pbuf;
		pushb->wqe[0] = (__u64)qp->wr_sq.cur_hdr;
		pushb->wqe[1] = (__u64)qp->wr_sq.cur_sqe;
		pushb->tail = *sq->dbtail;
		if (qp->cntx->db_push_mode == BNXT_RE_PUSH_MODE_PPP)
			bnxt_re_fill_ppp(pushb, qp, qp->wr_sq.cur_push_size,
					 qp->wr_sq.cur_slot_cnt);
		else
			bnxt_re_fill_push_wcb(qp, pushb, qp->wr_sq.cur_slot_cnt,
					      qp->wr_sq.cur_push_size);
	}
exit:
	bnxt_re_dp_spin_unlock(&sq->qlock);
	return err;
}

static void bnxt_re_send_wr_abort(struct ibv_qp_ex *ibvqp)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;

	bnxt_re_dp_spin_unlock(&sq->qlock);
}

static void bnxt_re_send_wr_set_sge(struct ibv_qp_ex *ibvqp, uint32_t lkey,
				    uint64_t addr, uint32_t length)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_sge *sge;

	if (unlikely(qp->wr_sq.error))
		return;
	sge = bnxt_re_get_hwqe(sq, qp->wr_sq.cur_slot_cnt++);
	bnxt_re_update_sge(sge, lkey, addr, length);
	if (qp->qptyp == IBV_QPT_UD) {
		qp->wr_sq.cur_hdr->lhdr.qkey_len |= htole32(length);
	} else {
		if ((qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_FETCH_AND_ADD) &&
		    (qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_CMP_AND_SWP))
			qp->wr_sq.cur_hdr->lhdr.qkey_len = htole32(length);
	}
	if (BNXT_RE_MSN_TBL_EN(qp->cntx))
		bnxt_re_fill_psns_for_msntbl(qp, length, *sq->dbtail, qp->wr_sq.cur_opcode);
	else
		bnxt_re_fill_psns(qp, length, *sq->dbtail, qp->wr_sq.cur_opcode);

	qp->wr_sq.used_slot_cnt = 3;
	bnxt_re_update_swqe(ibvqp, qp, length);
	qp->wr_sq.cur_wqe_cnt++;
	bnxt_re_set_wr_hdr_flags(qp, ibvqp->wr_flags);
}

static void bnxt_re_send_wr_set_sge_list(struct ibv_qp_ex *ibvqp, size_t nsge,
					 const struct ibv_sge *sgl)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	uint32_t i, len = 0;
	struct bnxt_re_sge *sge;

	if (unlikely(qp->wr_sq.error))
		return;
	if ((qp->wr_sq.cur_opcode == IBV_WR_ATOMIC_FETCH_AND_ADD) ||
	    (qp->wr_sq.cur_opcode == IBV_WR_ATOMIC_CMP_AND_SWP)) {
		qp->wr_sq.error = -EINVAL;
		return;
	}
	if (bnxt_re_is_que_full(sq, nsge)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	sge = (struct bnxt_re_sge *)bnxt_re_get_hwqe_no_wrap(sq, qp->wr_sq.cur_slot_cnt, nsge);
	if (likely(sge)) {
		qp->wr_sq.cur_slot_cnt += nsge;
		for (i = 0; i < nsge; i++) {
			bnxt_re_update_sge(sge, sgl[i].lkey, sgl[i].addr, sgl[i].length);
			len += sgl[i].length;
			sge++;
		}
	} else {
		for (i = 0; i < nsge; i++) {
			sge = bnxt_re_get_hwqe(sq, qp->wr_sq.cur_slot_cnt++);
			bnxt_re_update_sge(sge, sgl[i].lkey, sgl[i].addr, sgl[i].length);
			len += sgl[i].length;
		}
	}
	if (qp->qptyp == IBV_QPT_UD) {
		qp->wr_sq.cur_hdr->lhdr.qkey_len |= htole32(len);
	} else {
		if ((qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_FETCH_AND_ADD) &&
		    (qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_CMP_AND_SWP))
			qp->wr_sq.cur_hdr->lhdr.qkey_len = htole32(len);
	}
	if (BNXT_RE_MSN_TBL_EN(qp->cntx))
		bnxt_re_fill_psns_for_msntbl(qp, len, *sq->dbtail, qp->wr_sq.cur_opcode);
	else
		bnxt_re_fill_psns(qp, len, *sq->dbtail, qp->wr_sq.cur_opcode);

	qp->wr_sq.used_slot_cnt = nsge + 2;
	bnxt_re_update_swqe(ibvqp, qp, len);
	qp->wr_sq.cur_wqe_cnt++;
	bnxt_re_set_wr_hdr_flags(qp, ibvqp->wr_flags);
}

static void bnxt_re_send_wr_set_inline_data(struct ibv_qp_ex *ibvqp,
					    void *addr, size_t length)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_push_buffer *pushb = NULL;
	struct ibv_data_buf ibv_buf;
	uint32_t wrd_slot_cnt;
	uint32_t len = 0;

	if (unlikely(qp->wr_sq.error))
		return;
	if (qp->push_st_en && length < qp->max_push_sz) {
		pushb = (struct bnxt_re_push_buffer *)qp->pbuf;
		pushb->qpid = qp->qpid;
		pushb->st_idx = *sq->dbtail;
		qp->wr_sq.cur_push_wqe = true;
	}
	ibv_buf.addr = addr;
	ibv_buf.length = length;
	wrd_slot_cnt = (length + MSG_LEN_ADJ_TO_BYTES) >> SLOTS_RSH_TO_NUM_WQE;
	len = bnxt_re_put_wr_inline(sq, &qp->wr_sq.cur_slot_cnt, pushb, 1, &ibv_buf, &length);
	if (qp->qptyp == IBV_QPT_UD) {
		qp->wr_sq.cur_hdr->lhdr.qkey_len |= htole32(len);
	} else {
		if ((qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_FETCH_AND_ADD) &&
		    (qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_CMP_AND_SWP))
			qp->wr_sq.cur_hdr->lhdr.qkey_len = htole32(len);
	}
	if (BNXT_RE_MSN_TBL_EN(qp->cntx))
		bnxt_re_fill_psns_for_msntbl(qp, len, *sq->dbtail, qp->wr_sq.cur_wqe_cnt);
	else
		bnxt_re_fill_psns(qp, len, *sq->dbtail, qp->wr_sq.cur_opcode);
	qp->wr_sq.used_slot_cnt = wrd_slot_cnt + 2;
	bnxt_re_update_swqe(ibvqp, qp, len);
	qp->wr_sq.cur_wqe_cnt++;
	qp->wr_sq.cur_push_size += length;
	bnxt_re_set_wr_hdr_flags(qp, ibvqp->wr_flags);
}

static void bnxt_re_send_wr_set_inline_data_list(struct ibv_qp_ex *ibvqp, size_t num_buf,
						 const struct ibv_data_buf *buf_list)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_push_buffer *pushb = NULL;
	uint32_t i, num, len = 0;
	size_t msg_len = 0;

	if (unlikely(qp->wr_sq.error))
		return;
	/* Get the total message length */
	for (i = 0; i < num_buf; i++)
		msg_len += buf_list[i].length;
	if (qp->push_st_en && msg_len < qp->max_push_sz) {
		pushb = (struct bnxt_re_push_buffer *)qp->pbuf;
		pushb->qpid = qp->qpid;
		pushb->st_idx = *sq->dbtail;
		qp->wr_sq.cur_push_wqe = true;
	}
	num = (msg_len + MSG_LEN_ADJ_TO_BYTES) >> SLOTS_RSH_TO_NUM_WQE;
	/* check the queue full including header slots */
	if (bnxt_re_is_que_full(sq, num + 2)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	len = bnxt_re_put_wr_inline(sq, &qp->wr_sq.cur_slot_cnt, pushb,
				    num_buf, buf_list, &msg_len);
	if (qp->qptyp == IBV_QPT_UD) {
		qp->wr_sq.cur_hdr->lhdr.qkey_len |= htole32(len);
	} else {
		if ((qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_FETCH_AND_ADD) &&
		    (qp->wr_sq.cur_opcode != IBV_WR_ATOMIC_CMP_AND_SWP))
			qp->wr_sq.cur_hdr->lhdr.qkey_len = htole32(len);
	}
	if (BNXT_RE_MSN_TBL_EN(qp->cntx))
		bnxt_re_fill_psns_for_msntbl(qp, len, *sq->dbtail, qp->wr_sq.cur_opcode);
	else
		bnxt_re_fill_psns(qp, len, *sq->dbtail, qp->wr_sq.cur_opcode);
	qp->wr_sq.used_slot_cnt = num + 2;
	bnxt_re_update_swqe(ibvqp, qp, len);
	qp->wr_sq.cur_wqe_cnt++;
	qp->wr_sq.cur_push_size += msg_len;
	bnxt_re_set_wr_hdr_flags(qp, ibvqp->wr_flags);
}

static void bnxt_re_send_wr_set_ud_addr(struct ibv_qp_ex *ibvqp, struct ibv_ah *ibah,
					uint32_t remote_qpn, uint32_t remote_qkey)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_ah *ah;
	uint64_t qkey;

	if (unlikely(!ibah)) {
		qp->wr_sq.error = -EINVAL;
		return;
	}
	ah = to_bnxt_re_ah(ibah);
	qkey = remote_qkey;
	qp->wr_sq.cur_hdr->lhdr.qkey_len |= htole64(qkey << 32);
	qp->wr_sq.cur_sqe->dst_qp = htole32(remote_qpn);
	qp->wr_sq.cur_sqe->avid = htole32(ah->avid & 0xFFFFF);
}

static void bnxt_re_send_wr_send(struct ibv_qp_ex *ibvqp)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;

	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	bnxt_re_update_wr_common_hdr(qp, IBV_WR_SEND);
}

static void bnxt_re_send_wr_send_imm(struct ibv_qp_ex *ibvqp, __be32 imm_data)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;

	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	bnxt_re_update_wr_common_hdr(qp, IBV_WR_SEND_WITH_IMM);
	qp->wr_sq.cur_hdr->key_immd = htole32(be32toh(imm_data));
}

static void bnxt_re_send_wr_rdma_read(struct ibv_qp_ex *ibvqp, uint32_t rkey, uint64_t raddr)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_rdma *rsqe;

	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	bnxt_re_update_wr_common_hdr(qp, IBV_WR_RDMA_READ);
	rsqe = (struct bnxt_re_rdma *)qp->wr_sq.cur_sqe;
	rsqe->rva = htole64(raddr);
	rsqe->rkey = htole32(rkey);
}

static void bnxt_re_send_wr_rdma_write(struct ibv_qp_ex *ibvqp, uint32_t rkey, uint64_t raddr)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_rdma *rsqe;

	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	bnxt_re_update_wr_common_hdr(qp, IBV_WR_RDMA_WRITE);
	rsqe = (struct bnxt_re_rdma *)qp->wr_sq.cur_sqe;
	rsqe->rva = htole64(raddr);
	rsqe->rkey = htole32(rkey);
}

static void bnxt_re_send_wr_rdma_write_imm(struct ibv_qp_ex *ibvqp, uint32_t rkey, uint64_t raddr,
					   __be32 imm_data)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_rdma *rsqe;

	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	bnxt_re_update_wr_common_hdr(qp, IBV_WR_RDMA_WRITE_WITH_IMM);
	qp->wr_sq.cur_hdr->key_immd = htole32(be32toh(imm_data));
	rsqe = (struct bnxt_re_rdma *)qp->wr_sq.cur_sqe;
	rsqe->rva = htole64(raddr);
	rsqe->rkey = htole32(rkey);
}

static void bnxt_re_send_wr_atomic_cmp_swp(struct ibv_qp_ex *ibvqp, uint32_t rkey,
					   uint64_t raddr, uint64_t compare, uint64_t swap)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_atomic *sqe;

	if (unlikely(!qp->cap.is_atomic_cap)) {
		qp->wr_sq.error = -EINVAL;
		return;
	}
	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}
	bnxt_re_update_wr_common_hdr(qp, IBV_WR_ATOMIC_CMP_AND_SWP);
	qp->wr_sq.cur_hdr->key_immd = htole32(rkey);
	qp->wr_sq.cur_hdr->lhdr.rva = htole64(raddr);
	sqe = (struct bnxt_re_atomic *)qp->wr_sq.cur_sqe;
	sqe->cmp_dt = htole64(compare);
	sqe->swp_dt = htole64(swap);
}

static void bnxt_re_send_wr_atomic_fetch_add(struct ibv_qp_ex *ibvqp, uint32_t rkey,
					     uint64_t raddr, uint64_t add)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp((struct ibv_qp *)ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_atomic *sqe;

	if (unlikely(!qp->cap.is_atomic_cap)) {
		qp->wr_sq.error = -EINVAL;
		return;
	}
	if (bnxt_re_is_que_full(sq, SEND_SGE_MIN_SLOTS)) {
		qp->wr_sq.error = ENOMEM;
		return;
	}

	bnxt_re_update_wr_common_hdr(qp, IBV_WR_ATOMIC_FETCH_AND_ADD);
	qp->wr_sq.cur_hdr->key_immd = htole32(rkey);
	qp->wr_sq.cur_hdr->lhdr.rva = htole64(raddr);
	sqe = (struct bnxt_re_atomic *)qp->wr_sq.cur_sqe;
	sqe->swp_dt = htole64(add);
}

static void bnxt_re_set_qp_ex_ops(struct  bnxt_re_qp *qp, uint64_t ops_flags)
{
	struct ibv_qp_ex *ibqp = &qp->vqp.qp_ex;

	if (ops_flags & IBV_QP_EX_WITH_RDMA_WRITE)
		ibqp->wr_rdma_write = bnxt_re_send_wr_rdma_write;
	if (ops_flags & IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM)
		ibqp->wr_rdma_write_imm = bnxt_re_send_wr_rdma_write_imm;
	if (ops_flags & IBV_QP_EX_WITH_SEND)
		ibqp->wr_send = bnxt_re_send_wr_send;
	if (ops_flags & IBV_QP_EX_WITH_SEND_WITH_IMM)
		ibqp->wr_send_imm = bnxt_re_send_wr_send_imm;
	if (ops_flags & IBV_QP_EX_WITH_RDMA_READ)
		ibqp->wr_rdma_read = bnxt_re_send_wr_rdma_read;
	if (ops_flags & IBV_QP_EX_WITH_ATOMIC_CMP_AND_SWP)
		ibqp->wr_atomic_cmp_swp = bnxt_re_send_wr_atomic_cmp_swp;
	if (ops_flags & IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD)
		ibqp->wr_atomic_fetch_add = bnxt_re_send_wr_atomic_fetch_add;

	ibqp->wr_set_sge = bnxt_re_send_wr_set_sge;
	ibqp->wr_set_sge_list = bnxt_re_send_wr_set_sge_list;
	ibqp->wr_set_inline_data = bnxt_re_send_wr_set_inline_data;
	ibqp->wr_set_inline_data_list = bnxt_re_send_wr_set_inline_data_list;
	ibqp->wr_set_ud_addr = bnxt_re_send_wr_set_ud_addr;
	ibqp->wr_start = bnxt_re_send_wr_start;
	ibqp->wr_complete = bnxt_re_send_wr_complete;
	ibqp->wr_abort = bnxt_re_send_wr_abort;
}
#endif

static bool exp_mode_enable(void)
{
	char *env;

	env = getenv("BNXT_EXP_MODE");
	if (env)
		return strcmp(env, "1") ? false : true;

	return false;
}

struct ibv_qp *__bnxt_re_create_qp(struct ibv_context *ibvctx,
				   struct ibv_qp_init_attr_ex *attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_qp_resp resp = {};
	struct ibv_device_attr *devattr;
	struct bnxt_re_qp_req req = {};
	struct bnxt_re_qattr qattr[2];
	struct bnxt_re_qpcap *cap;
	struct bnxt_re_dev *rdev;
	struct bnxt_re_qp *qp;
	void *mem;

	if (bnxt_re_check_qp_limits(cntx, attr))
		return NULL;

	memset(qattr, 0, (2 * sizeof(*qattr)));

	/*
	 * Express mode needs to be activated through an environment variable,
	 * on a per QP basis. User may mix express and non-express modes across
	 * different QPs.
	 */
	if (cntx->exp_supported && exp_mode_enable()) {
		req.exp_mode = 1;
		qattr[BNXT_RE_QATTR_SQ_INDX].exp_mode = true;
		qattr[BNXT_RE_QATTR_RQ_INDX].exp_mode = true;
	}

	mem = bnxt_re_alloc_qpslab(cntx, attr, qattr);
	if (!mem)
		return NULL;
	qp = bnxt_re_get_obj(mem, sizeof(*qp));
	if (!qp)
		goto fail;
	qp->ibvqp = &qp->vqp.qp;
	qp->mem = mem;

	qp->cctx = cntx->cctx;

	qp->cntx = cntx;
	qp->mtu = BNXT_RE_QP_MTU_DEFAULT;
	qp->re_pd = to_bnxt_re_pd(attr->pd);
	qp->qpmode = cntx->modes & BNXT_RE_WQE_MODE_VARIABLE;
	qp->exp_mode = req.exp_mode;

	if (cntx->inline_optimizer_supported &&
	    (qp->re_pd->comp_mask & BNXT_RE_COMP_MASK_PD_INLINE_OPT_MW_RKEY_VALID))
		qp->inline_opt_enabled = 1;

	/* alloc queue pointers */
	if (bnxt_re_alloc_queue_ptr(qp, attr))
		goto fail;
	/* alloc queues */
	if (bnxt_re_alloc_queues(qp, attr, qattr))
		goto fail;
	/* Fill ibv_cmd */
	cap = &qp->cap;
	req.qpsva = (uint64_t)qp->jsqq->hwque->va;
	req.qp_handle = (uint64_t)qp;
	req.sqprodva = (uint64_t)&qp->jsqq->hwque->tail;
	req.sqconsva = (uint64_t)&qp->jsqq->hwque->head;
	if (qp->jrqq) {
		req.qprva = (uint64_t)qp->jrqq->hwque->va;
		req.rqprodva = (uint64_t)&qp->jrqq->hwque->tail;
		req.rqconsva = (uint64_t)&qp->jrqq->hwque->head;
	} else {
		req.qprva = 0;
		req.rqprodva = 0;
		req.rqconsva = 0;
	}

	/* coverity[UNUSED_VALUE:SUPPRESS] */
	req.comp_mask |= BNXT_RE_QP_REQ_MASK_DUMP_QP_INDEX;
	if (qp->qpmode == BNXT_RE_WQE_MODE_VARIABLE)
		req.sq_slots = qattr[BNXT_RE_QATTR_SQ_INDX].slots;

	if (ibv_cmd_create_qp_ex_compat(ibvctx, &qp->vqp, sizeof(qp->vqp),
					attr, &req.cmd, sizeof(req), &resp.resp,
					sizeof(resp)))
		goto fail;
#ifdef HAVE_IBV_WR_API
	if (attr->comp_mask & IBV_QP_INIT_ATTR_SEND_OPS_FLAGS) {
		bnxt_re_set_qp_ex_ops(qp, attr->send_ops_flags);
		qp->vqp.comp_mask |= VERBS_QP_EX;
	}
#endif
	qp->qpid = resp.qpid;
	qp->dbc_dt = !!resp.hdbr_dt;
	if (resp.comp_mask & BNXT_RE_QP_RESP_MASK_HDBR_KADDR_QP)
		qp->dbc_sq = bnxt_re_hdbr_map_dbc(ibvctx, resp.hdbr_kaddr_sq);
	if (resp.comp_mask & BNXT_RE_QP_RESP_MASK_HDBR_KADDR_QP)
		qp->dbc_rq = bnxt_re_hdbr_map_dbc(ibvctx, resp.hdbr_kaddr_rq);
	qp->qptyp = attr->qp_type;
	qp->qpst = IBV_QPS_RESET;
	qp->scq = to_bnxt_re_cq(attr->send_cq);
	qp->rcq = to_bnxt_re_cq(attr->recv_cq);
	if (attr->srq)
		qp->srq = to_bnxt_re_srq(attr->srq);
	qp->udpi = &cntx->udpi;
	qp->rand.seed = qp->qpid;
	qp->sq_shadow_db_key = BNXT_RE_DB_KEY_INVALID;
	qp->rq_shadow_db_key = BNXT_RE_DB_KEY_INVALID;
	qp->sq_msn = 0;

	rdev = cntx->rdev;
	devattr = &rdev->devattr;
	cap->max_ssge = attr->cap.max_send_sge;
	cap->max_rsge = attr->cap.max_recv_sge;
	cap->max_inline = attr->cap.max_inline_data;
	cap->sqsig = attr->sq_sig_all;
	cap->is_atomic_cap = devattr->atomic_cap;
	INIT_DBLY_LIST_NODE(&qp->snode);
	INIT_DBLY_LIST_NODE(&qp->rnode);
	INIT_DBLY_LIST_NODE(&qp->dbnode);

	/* For BNXT_RE_PUSH_MODE_PPP, push will be negotiated at modify qp */
	/* TBD - diff between upstream and in-hosue. Check */
	if (cntx->db_push_mode == BNXT_RE_PUSH_MODE_WCB) {
		qp->push_st_en = 1;
		if (qp->qpmode == BNXT_RE_WQE_MODE_VARIABLE)
			qp->max_push_sz = BNXT_RE_MAX_WCB_SIZE_VAR_WQE;
		else
			qp->max_push_sz = BNXT_RE_MAX_INLINE_SIZE;

		bnxt_re_get_pbuf(qp);
	}

#ifdef BNXT_RE_ENABLE_DEFFER_DB
	if (cntx->deferred_db_enabled)
		bnxt_re_db_ctrl_init(&qp->sq_db_ctrl, (void(*)(void *))bnxt_re_ring_sq_db);
		bnxt_re_db_ctrl_init(&qp->rq_db_ctrl, (void(*)(void *))bnxt_re_ring_rq_db);
#endif
	return qp->ibvqp;
fail:
	bnxt_re_free_mem(mem, bnxt_re_get_parent_domain(attr->pd));
	return NULL;
}

#ifdef HAVE_IBV_WR_API
struct ibv_qp *bnxt_re_create_qp_ex(struct ibv_context *ibvctx,
				    struct ibv_qp_init_attr_ex *attr)
{
	return __bnxt_re_create_qp(ibvctx, attr);
}
#endif

struct ibv_qp *bnxt_re_create_qp(struct ibv_pd *ibvpd,
				 struct ibv_qp_init_attr *attr)
{
	struct ibv_qp_init_attr_ex attr_ex;
	struct ibv_qp *qp;

	memset(&attr_ex, 0, sizeof(attr_ex));
	memcpy(&attr_ex, attr, sizeof(*attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;
	qp = __bnxt_re_create_qp(ibvpd->context, &attr_ex);
	if (qp)
		memcpy(attr, &attr_ex, sizeof(*attr));
	return qp;
}

int bnxt_re_modify_qp(struct ibv_qp *ibvqp, struct ibv_qp_attr *attr,
		      int attr_mask)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	int rc;

#ifdef HAVE_IBV_CMD_MODIFY_QP_EX
	struct bnxt_re_modify_qp_ex_resp resp = {};
	struct bnxt_re_modify_qp_ex_req req = {};
	bool can_issue_mqp_ex = false;

	if (bnxt_re_is_mqp_ex_supported(qp->cntx)) {
		can_issue_mqp_ex = true;
		/* Request for PPP */
		if (can_request_ppp(qp, attr, attr_mask)) {
			req.comp_mask |= BNXT_RE_COMP_MASK_MQP_EX_PPP_REQ_EN;
			req.dpi = qp->udpi->wcdpi;
		}
		if (attr_mask & IBV_QP_PATH_MTU)
			req.comp_mask |= BNXT_RE_COMP_MASK_MQP_EX_PATH_MTU_MASK;
	}
#endif
	rc = ibv_cmd_modify_qp_compat(ibvqp, attr, attr_mask
#ifdef HAVE_IBV_CMD_MODIFY_QP_EX
				      , can_issue_mqp_ex, &req, &resp
#endif
				     );
	if (!rc) {
		if (attr_mask & IBV_QP_STATE) {
			qp->qpst = attr->qp_state;
			/* transition to reset */
			if (qp->qpst == IBV_QPS_RESET) {
				qp->jsqq->hwque->head = 0;
				qp->jsqq->hwque->tail = 0;
				*qp->jsqq->hwque->dbtail = 0;
				qp->jsqq->start_idx = 0;
				qp->jsqq->last_idx = 0;
				bnxt_re_cleanup_cq(qp, qp->scq);
				if (qp->jrqq) {
					qp->jrqq->hwque->head = 0;
					qp->jrqq->hwque->tail = 0;
					*qp->jrqq->hwque->dbtail = 0;
					qp->jrqq->start_idx = 0;
					qp->jrqq->last_idx = 0;
					if (qp->rcq != qp->scq)
						bnxt_re_cleanup_cq(qp, qp->rcq);
				}
			}

			/*
			 * When the QP moves to INIT to RTR in the modify_qp
			 * call, firmware has a workaround to update the context
			 * field with CDUDMA read/write. During the QP INIT
			 * state in the post_receive library needs to ensure
			 * no doorbell is rung to work this properly. And once
			 * QP moves to RTR ring the doorbell if the producer
			 * index is present.
			 */
			if (qp->qpst == IBV_QPS_RTR && !qp->srq && qp->jrqq) {
				if (qp->jrqq->hwque->tail) {
					bnxt_re_dp_spin_lock(&qp->jrqq->hwque->qlock);
					bnxt_re_ring_rq_db(qp);
					bnxt_re_dp_spin_unlock(&qp->jrqq->hwque->qlock);
				}
			}
#ifdef HAVE_IBV_CMD_MODIFY_QP_EX
			/* Copy if PUSH was enabled */
			if (resp.comp_mask & BNXT_RE_COMP_MASK_MQP_EX_PPP_REQ_EN_MASK) {
				qp->push_st_en = BNXT_RE_COMP_MASK_MQP_EX_PPP_REQ_EN;
				/* Set the next posting state
				 * based on current h/w state
				 */
				qp->push_st_en |=
					!(!!(resp.ppp_st_idx &
					     BNXT_RE_COMP_MASK_MQP_EX_PPP_STATE)) <<
					 BNXT_RE_PPP_ST_SHIFT;
				qp->ppp_idx = resp.ppp_st_idx &
						BNXT_RE_COMP_MASK_MQP_EX_PPP_IDX_MASK;
				if (qp->qpmode == BNXT_RE_WQE_MODE_VARIABLE)
					qp->max_push_sz = BNXT_RE_MAX_PPP_SIZE_VAR_WQE;
				else
					qp->max_push_sz = BNXT_RE_MAX_INLINE_SIZE;
				bnxt_re_get_pbuf(qp);
			}
#endif
		}

		if (attr_mask & IBV_QP_SQ_PSN)
			qp->sq_psn = attr->sq_psn;

		if (resp.comp_mask & BNXT_RE_COMP_MASK_MQP_EX_PATH_MTU_MASK)
			qp->mtu = resp.path_mtu;
		else if (attr_mask & IBV_QP_PATH_MTU)
			qp->mtu = (0x80 << attr->path_mtu);
	}

	return rc;
}

int bnxt_re_query_qp(struct ibv_qp *ibvqp, struct ibv_qp_attr *attr,
		     int attr_mask, struct ibv_qp_init_attr *init_attr)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct ibv_query_qp cmd = {};
	int rc;

	rc = ibv_cmd_query_qp(ibvqp, attr, attr_mask, init_attr,
			      &cmd, sizeof(cmd));
	if (!rc) {
		qp->qpst = ibvqp->state;
		qp->cap.max_inline = init_attr->cap.max_inline_data;
	}

	return rc;
}

int bnxt_re_destroy_qp(struct ibv_qp *ibvqp)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct bnxt_re_mem *mem;
	int status;

#ifdef BNXT_RE_ENABLE_DEFFER_DB
	if (qp->cntx && qp->cntx->deferred_db_enabled)
		bnxt_re_db_ctrl_destroy(&qp->sq_db_ctrl);
		bnxt_re_db_ctrl_destroy(&qp->rq_db_ctrl);
#endif

	qp->qpst = IBV_QPS_RESET;
	bnxt_re_hdbr_unmap_dbc(qp->dbc_sq);
	bnxt_re_hdbr_unmap_dbc(qp->dbc_rq);
	status = ibv_cmd_destroy_qp(ibvqp);
	if (status) {
		return status;
	}
	if (qp->cntx->db_push_mode == BNXT_RE_PUSH_MODE_PPP && qp->push_st_en) {
		pthread_mutex_lock(&qp->cntx->shlock);
		qp->cntx->ppp_cnt--;
		pthread_mutex_unlock(&qp->cntx->shlock);
	}
	if (qp->pbuf) {
		bnxt_re_put_pbuf(qp);
		qp->pbuf = NULL;
	}
	bnxt_re_cleanup_cq(qp, qp->rcq);
	if (qp->scq != qp->rcq)
		bnxt_re_cleanup_cq(qp, qp->scq);
	if (qp->parent_domain)
		atomic_fetch_sub(&qp->parent_domain->refcount, 1);
	mem = qp->mem;
	bnxt_re_free_mem(mem, qp->parent_domain);
	return 0;
}

static void bnxt_re_put_rx_sge(struct bnxt_re_qp *qp,
			       struct bnxt_re_exp_db *db,
			       struct bnxt_re_queue *que, uint32_t *idx,
			       struct ibv_sge *sgl, int nsg)
{
	struct bnxt_re_sge *sge;
	int indx;
#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
	return bnxt_re_put_rx_sge_test(qp, db, que, idx, sgl, nsg);
#endif
	for (indx = 0; indx < nsg; indx++) {
		sge = qp->exp_mode ? &db->wqe[indx + BNXT_RE_EXP_RQ_SGE_OFFSET] :
			bnxt_re_get_hwqe(que, *idx);
		(*idx)++;
		sge->pa = htole64(sgl[indx].addr);
		sge->lkey = htole32(sgl[indx].lkey);
		sge->length = htole32(sgl[indx].length);
	}
}

static unsigned int bnxt_re_put_tx_sge(struct bnxt_re_qp *qp, struct bnxt_re_queue *que,
				       uint32_t *idx,
				       struct ibv_sge *sgl, int nsg)
{
	struct bnxt_re_sge *sge;
	int indx;
	int len;

	len = 0;
	for (indx = 0; indx < nsg; indx++) {
		sge = bnxt_re_get_hwqe(que, (*idx)++);
		sge->pa = htole64(sgl[indx].addr);
		sge->lkey = htole32(sgl[indx].lkey);
		sge->length = htole32(sgl[indx].length);
		len += sgl[indx].length;
	}
	return len;
}

static inline unsigned int bnxt_re_calc_inline_len(struct ibv_send_wr *swr)
{
	int illen, indx;

	illen = 0;
	for (indx = 0; indx < swr->num_sge; indx++)
		illen += swr->sg_list[indx].length;
	return illen;
}

static inline int inline_opt_required_slots(int slots)
{
	if (slots <= INLINE_OPT_MIN_SLOTS || slots >= INLINE_OPT_MAX_SLOTS)
		return 0;

	return (((INLINE_OPT_MAX_SLOTS - slots) + 1) & ~1);
}

static unsigned int bnxt_re_put_inline(struct bnxt_re_qp *qp, struct bnxt_re_queue *que,
				       uint32_t *idx,
				       struct bnxt_re_push_buffer *pbuf,
				       struct ibv_sge *sgl, uint32_t nsg,
				       unsigned int *msg_len, int *slots)
{
	unsigned int len, t_len, offt = 0;
	struct bnxt_re_pd *pd = qp->re_pd;
	int t_cplen = 0, cplen;
	int alsize, indx, num;
	bool pull_dst = true;
	void *il_dst, *til_dst;
	void *il_src;
	int tmp_num = 0;
	int add_slots = 0;

	if (qp->inline_opt_enabled)
		add_slots = inline_opt_required_slots(*slots);

	t_len = 0;
	num = (*msg_len + 15) >> 4;
	tmp_num = num;
	num += add_slots;
	il_dst = bnxt_re_get_hwqe_no_wrap(que, *idx, num);
	if (likely(il_dst)) {
		/* If queue won't wrap */
		til_dst = il_dst;
		if (pbuf)
			pbuf->wqe[(*idx)++] = (__u64)il_dst;
		for (indx = 0; indx < nsg; indx++) {
			len = sgl[indx].length;
			il_src = (void *)sgl[indx].addr;
			t_len += len;
			memcpy(il_dst, il_src, len);
			il_dst += len;
		}
		if (add_slots) {
			il_dst = (void *)((char *)til_dst + (tmp_num << 4));
			il_src = pd->inline_opt_pad_wqes->va_head;
			memcpy(il_dst, il_src, (add_slots << 4));
			(*slots) += add_slots;
		}
		return t_len;
	}
	/* move on to queue wrap case */
	alsize = sizeof(struct bnxt_re_sge);
	for (indx = 0; indx < nsg; indx++) {
		len = sgl[indx].length;
		il_src = (void *)sgl[indx].addr;
		t_len += len;
		while (len) {
			if (pull_dst) {
				pull_dst = false;
				il_dst = bnxt_re_get_hwqe(que, (*idx)++);
				if (pbuf)
					pbuf->wqe[*idx - 1] = (__u64)il_dst;
				t_cplen = 0;
				offt = 0;
			}
			cplen = MIN(len, alsize);
			cplen = MIN(cplen,(alsize - offt));
			memcpy(il_dst, il_src, cplen);
			t_cplen += cplen;
			il_src += cplen;
			il_dst += cplen;
			offt += cplen;
			len -= cplen;
			if (t_cplen == alsize)
				pull_dst = true;
		}
	}
	if (add_slots) {
		il_src = pd->inline_opt_pad_wqes->va_head;
		il_dst = bnxt_re_get_hwqe_no_wrap(que, *idx, add_slots);
		if (il_dst) {
			memcpy(il_dst, il_src, (add_slots << 4));
			(*slots) += add_slots;
		} else {
			for (indx = 0; indx < add_slots; indx++) {
				il_dst = bnxt_re_get_hwqe(que, (*idx)++);
				memcpy(il_dst, il_src, alsize);
				il_src += alsize;
			}
			(*slots) += add_slots;
		}
	}
	/* Use msg_len field to indicate whether the queue wrapped or not */
	*msg_len = 0;
	return t_len;
}

static bool bnxt_re_sq_has_compact_wqe(struct bnxt_re_qp *qp,
				       enum ibv_wr_opcode opcode)
{
	if (!qp->cctx->is_hsi_v3)
		return false;

	if (opcode == IBV_WR_LOCAL_INV)
		return true;

	if ((opcode == IBV_WR_SEND_WITH_IMM ||
	     opcode == IBV_WR_SEND_WITH_INV ||
	     opcode == IBV_WR_SEND) &&
	    qp->qptyp == IBV_QPT_RC)
		return true;

#ifdef HAVE_DRIVER1
	if (opcode == IBV_WR_DRIVER1)
		return true;
#endif

	return false;
}

static int bnxt_re_required_slots(struct bnxt_re_qp *qp, struct ibv_send_wr *wr,
				  uint32_t *wqe_sz, void **pbuf, unsigned int *ilsize)
{
	uint32_t wqe_byte;
	bool compact_wqe;

	compact_wqe = bnxt_re_sq_has_compact_wqe(qp, wr->opcode);
	if (wr->send_flags & IBV_SEND_INLINE) {
		*ilsize = bnxt_re_calc_inline_len(wr);
		if (*ilsize > qp->cap.max_inline)
			return -EINVAL;
		/* For exp mode, largest inline supported is 16B, i.e., size of 1 SGE */
		if (qp->exp_mode && *ilsize > sizeof(struct bnxt_re_sge))
			return -EINVAL;

		if (qp->push_st_en && *ilsize <= qp->max_push_sz &&
		    bnxt_re_db_ctrl_check_push(&qp->sq_db_ctrl))
			*pbuf = qp->pbuf;
		wqe_byte = get_aligned(*ilsize, sizeof(struct bnxt_re_sge))
					+ bnxt_re_get_sqe_hdr_sz(qp->cctx, compact_wqe);
	} else {
		wqe_byte = bnxt_re_calc_sq_wqe_sz(qp->cctx, wr->num_sge, 0, compact_wqe);
	}

	/* que->stride is always 2^4 = 16, thus using hard-coding */
	*wqe_sz = wqe_byte >> 4;
	if (qp->qpmode == BNXT_RE_WQE_MODE_STATIC)
		return 8;
	return *wqe_sz;
}

static inline void bnxt_re_set_hdr_flags(struct bnxt_re_bsqe *hdr,
					 struct bnxt_re_queue *sq,
					 struct ibv_send_wr *wr,
					 uint32_t slots, uint8_t sqsig,
					 uint8_t qp_type, bool is_hsi_v3,
					 uint32_t ilsize)
{
	uint32_t send_flags;
	uint32_t hdrval = 0;
	uint8_t opcd;

	send_flags = wr->send_flags;
#ifdef HAVE_DRIVER1
	/* IBV_WR_DRIVER1 is used for Broadcom UDP source port change through
	 * WQE feature.
	 * When the connection type is RC, fence needs to be set to ensure all
	 * previous WQEs are completed (by receiving the ACKs) before changing
	 * the UDP port.
	 * When the connection type is UD, since the HW cannot rely on ACK and
	 * therefore there is no guarantee that all previous WQEs are completed
	 * before the UDP port change takes place. In such case, we need to
	 * ensure fence is disabled. Otherwise, the TX pipeline will hang.
	 * Note: this opcode is limited to RC and UD only.
	 */
	if (unlikely(wr->opcode == IBV_WR_DRIVER1)) {
		if (qp_type == IBV_QPT_RC)
			send_flags |= IBV_SEND_FENCE;
		else
			send_flags &= ~IBV_SEND_FENCE;
	}
#endif

	if (send_flags & IBV_SEND_SIGNALED || sqsig)
		hdrval |= ((BNXT_RE_WR_FLAGS_SIGNALED & BNXT_RE_HDR_FLAGS_MASK)
			    << BNXT_RE_HDR_FLAGS_SHIFT);
	if (send_flags & IBV_SEND_FENCE)
		/*TODO: See when RD fence can be used. */
		hdrval |= ((BNXT_RE_WR_FLAGS_UC_FENCE & BNXT_RE_HDR_FLAGS_MASK)
			    << BNXT_RE_HDR_FLAGS_SHIFT);
	if (send_flags & IBV_SEND_SOLICITED)
		hdrval |= ((BNXT_RE_WR_FLAGS_SE & BNXT_RE_HDR_FLAGS_MASK)
			    << BNXT_RE_HDR_FLAGS_SHIFT);
	if (send_flags & IBV_SEND_INLINE) {
		hdrval |= ((BNXT_RE_WR_FLAGS_INLINE & BNXT_RE_HDR_FLAGS_MASK)
			    << BNXT_RE_HDR_FLAGS_SHIFT);
		if (is_hsi_v3) {
			/*
			 * Calculate the inline_length (Offset:0x0[27:24]),
			 * which is remainder of the last 16 bytes of data to
			 * be programmed into the header
			 */
			ilsize %= sizeof(struct bnxt_re_sge);

			if (ilsize)
				hdrval |= ((ilsize & BNXT_RE_HDR_IL_MASK)
					    << BNXT_RE_HDR_IL_SHIFT);
		}
	}
	hdrval |= (slots & BNXT_RE_HDR_WS_MASK) << BNXT_RE_HDR_WS_SHIFT;

        /* Fill opcode */
	opcd = is_hsi_v3 ? ibv_to_bnxt_re_wr_opcd_v3[wr->opcode] :
		ibv_to_bnxt_re_wr_opcd[wr->opcode];
#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
	opcd = bnxt_re_sq_hdr_val_test(sq, opcd);
#endif

	if (is_hsi_v3 && qp_type == IBV_QPT_UD) {
		switch (wr->opcode) {
		case IBV_WR_SEND_WITH_IMM:
			opcd = BNXT_RE_WR_OPCD_UDSEND_IMM_V3;
			break;
		case IBV_WR_SEND:
			opcd = BNXT_RE_WR_OPCD_UDSEND_V3;
			break;
#ifdef HAVE_DRIVER1
		case IBV_WR_DRIVER1:
			/* This opcode is the same for RC and UD */
			break;
#endif
		default:
			fprintf(stderr, DEV "Unsurported opcode 0x%x for UD QP\n", wr->opcode);
			break;
		}
	}

        hdrval |= (opcd & BNXT_RE_HDR_WT_MASK);
	hdr->rsv_ws_fl_wt = htole32(hdrval);
}

static unsigned int bnxt_re_build_tx_sge(struct bnxt_re_qp *qp, struct bnxt_re_queue *que,
					 uint32_t *idx,
					 struct bnxt_re_push_buffer *pbuf,
					 struct ibv_send_wr *wr,
					 unsigned int *max_il,
					 int *slots)
{
	if (wr->send_flags & IBV_SEND_INLINE)
		return bnxt_re_put_inline(qp, que, idx, pbuf, wr->sg_list,
					  wr->num_sge, max_il, slots);

#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
	return bnxt_re_put_tx_sge_test(que, idx, wr->sg_list, wr->num_sge);
#endif
	return bnxt_re_put_tx_sge(qp, que, idx, wr->sg_list, wr->num_sge);
}

static inline int bnxt_re_build_exp_tx_sge(struct bnxt_re_qp *qp,
					   struct bnxt_re_sge *sge,
					   struct ibv_send_wr *wr)
{
	if (wr->send_flags & IBV_SEND_INLINE) {
		memcpy(sge, (void *)wr->sg_list[0].addr, wr->sg_list[0].length);
		return wr->sg_list[0].length;
	}

	/* only 1 SGE is supported by SQ WQE in express mode */
	sge->pa = htole64(wr->sg_list[0].addr);
	sge->lkey = htole32(wr->sg_list[0].lkey);
	sge->length = htole32(wr->sg_list[0].length);

	return sge->length;
}

static int bnxt_re_build_ud_sqe(struct ibv_send_wr *wr,
				struct bnxt_re_bsqe *hdr,
				struct bnxt_re_send *sqe)
{
	struct bnxt_re_ah *ah;
	uint64_t qkey;

	if (!sqe) {
		fprintf(stderr, "%s: sqe missing\n", __func__);
		return -EINVAL;
	}

	ah = to_bnxt_re_ah(wr->wr.ud.ah);
	if (!wr->wr.ud.ah)
		return -EINVAL;
	qkey = wr->wr.ud.remote_qkey;
	hdr->lhdr.qkey_len &= ~htole64(0xFFFFFFFF00000000);
	hdr->lhdr.qkey_len |= htole64(qkey << 32);
	sqe->dst_qp = htole32(wr->wr.ud.remote_qpn);
	sqe->avid = htole32(ah->avid & 0xFFFFF);

	return 0;
}

static bool __atomic_not_supported(struct bnxt_re_qp *qp, struct ibv_send_wr *wr)
{
	/* Atomic capability disabled or the request has more than 1 SGE */
	return (!qp->cap.is_atomic_cap || wr->num_sge > 1);
}

static void bnxt_re_build_cns_sqe(struct ibv_send_wr *wr,
				  struct bnxt_re_bsqe *hdr,
				  void *hdr2)
{
	struct bnxt_re_atomic *sqe = hdr2;

	hdr->key_immd = htole32(wr->wr.atomic.rkey);
	hdr->lhdr.rva = htole64(wr->wr.atomic.remote_addr);
	sqe->cmp_dt = htole64(wr->wr.atomic.compare_add);
	sqe->swp_dt = htole64(wr->wr.atomic.swap);
}

static void bnxt_re_build_fna_sqe(struct ibv_send_wr *wr,
				  struct bnxt_re_bsqe *hdr,
				  void *hdr2)
{
	struct bnxt_re_atomic *sqe = hdr2;

	hdr->key_immd = htole32(wr->wr.atomic.rkey);
	hdr->lhdr.rva = htole64(wr->wr.atomic.remote_addr);
	sqe->swp_dt = htole64(wr->wr.atomic.compare_add);
}

static int bnxt_re_build_atomic_sqe(struct bnxt_re_qp *qp,
				    struct ibv_send_wr *wr,
				    struct bnxt_re_bsqe *hdr,
				    void *hdr2)
{
	if (__atomic_not_supported(qp, wr))
		return -EINVAL;
	switch (wr->opcode) {
	case IBV_WR_ATOMIC_CMP_AND_SWP:
		bnxt_re_build_cns_sqe(wr, hdr, hdr2);
		return 0;
	case IBV_WR_ATOMIC_FETCH_AND_ADD:
		bnxt_re_build_fna_sqe(wr, hdr, hdr2);
		return 0;
	default:
		return -EINVAL;
	}
}

#ifdef HAVE_WR_BIND_MW
static int bnxt_re_build_bind_sqe(struct bnxt_re_qp *qp,
				  struct ibv_send_wr *wr,
				  struct bnxt_re_bsqe *hdr,
				  void *hdr2)
{
	uint32_t hdr_acc = 0, hdr_typezb = 0;
	struct ibv_mw *mw = wr->bind_mw.mw;
	uint32_t acc, opcode, hdrval = 0;
	struct bnxt_re_bind *sqe = hdr2;
	uint64_t lkey;

	/* Build wqe header */
	opcode = ibv_to_bnxt_re_wr_opcd[wr->opcode];
	if (opcode == BNXT_RE_WR_OPCD_INVAL)
		return -EINVAL;

	acc = wr->bind_mw.bind_info.mw_access_flags;
	if (mw->type == IBV_MW_TYPE_1 && (acc & IBV_ACCESS_ZERO_BASED))
		return -EINVAL;

	if (acc & IBV_ACCESS_REMOTE_READ)
		hdr_acc |= BNXT_RE_WR_BIND_ACC_RRD;
	if (acc & IBV_ACCESS_REMOTE_WRITE)
		hdr_acc |= BNXT_RE_WR_BIND_ACC_RWR;
	if (acc & IBV_ACCESS_REMOTE_ATOMIC)
		hdr_acc |= BNXT_RE_WR_BIND_ACC_RAT;
	hdrval = (hdr_acc << BNXT_RE_WR_BIND_ACC_SHIFT);
	hdrval |= (opcode & BNXT_RE_HDR_WT_MASK);
	/* Set unconditional fence for bind opcode to complete
	 * with guarantee if completions was requested.
	 */
	if (!qp->cctx->chip_is_p5_plus)
		hdrval |= ((BNXT_RE_WR_FLAGS_UC_FENCE & BNXT_RE_HDR_FLAGS_MASK)
			   << BNXT_RE_HDR_FLAGS_SHIFT);
	hdr->rsv_ws_fl_wt |= htole32(hdrval);

	if (mw->type == IBV_MW_TYPE_2) {
		hdr_typezb |= BNXT_RE_MEMW_TYPE_2;
		if (acc & IBV_ACCESS_ZERO_BASED)
			hdr_typezb |= BNXT_RE_WR_BIND_ACC_ZBVA;
	}
	hdr->key_immd = htole32(hdr_typezb);

	/* Fill rest of the WQE */
	hdr->lhdr.lkey_plkey = htole32(wr->bind_mw.bind_info.mr->lkey);
	lkey = wr->bind_mw.rkey;
	hdr->lhdr.lkey_plkey |= htole64(lkey << 32);
	sqe->va = htole64(wr->bind_mw.bind_info.addr);
	sqe->len = htole64(wr->bind_mw.bind_info.length);

	return 0;
}
#endif

#ifdef HAVE_LOCAL_INV
static int bnxt_re_build_inval_sqe(struct bnxt_re_qp *qp,
				   struct ibv_send_wr *wr,
				   struct bnxt_re_bsqe *hdr,
				   void *hdr2)
{
	struct bnxt_re_bind *sqe = hdr2;
	uint32_t hdrval = 0;

	/* special case: lower 16B must be all 0s. */
	hdr->key_immd = htole32(wr->imm_data);
	/* rest of sqe is unused. */
	memset(sqe, 0, sizeof(struct bnxt_re_sge));
	/* Set unconditional fence for local-invalidate opcode to
	 * complete with guarantee if completion was requested.
	 */
	if (!qp->cctx->chip_is_p5_plus) {
		hdrval = ((BNXT_RE_WR_FLAGS_UC_FENCE & BNXT_RE_HDR_FLAGS_MASK)
			   << BNXT_RE_HDR_FLAGS_SHIFT);
		hdr->rsv_ws_fl_wt |= htole32(hdrval);
	}
	return 0;
}
#endif

void bnxt_re_force_rts2rts(struct bnxt_re_qp *qp)
{
	struct ibv_qp_attr attr = {};
	int attr_mask;
	/* WA for Bug 9208 */
	attr_mask = IBV_QP_STATE;
	attr.qp_state = IBV_QPS_RTS;
	bnxt_re_modify_qp(qp->ibvqp, &attr, attr_mask);
	qp->wqe_cnt = 0;
}

static int post_send_exp(struct ibv_qp *ibvqp, struct ibv_send_wr *wr,
			 struct ibv_send_wr **bad)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_bsqe_v3 *hdr_v3;
	union lower_shdr_v3 *lhdr_v3;
	int slots, ret = 0, len = 0;
	uint32_t swq_idx, wqe_size;
	struct bnxt_re_wrid *wrid;
	struct bnxt_re_rdma *rsqe;
	struct bnxt_re_exp_db db;
	struct bnxt_re_bsqe *hdr;
	struct bnxt_re_send *sqe;
	struct bnxt_re_sge *sge;
	unsigned int ilsize = 0;

	bnxt_re_dp_spin_lock(&sq->qlock);
	while (wr) {
		slots = bnxt_re_required_slots(qp, wr, &wqe_size, NULL, &ilsize);
		if (unlikely(slots < 0 || bnxt_re_is_que_full(sq, slots)) ||
		    wr->num_sge > 1) {
			*bad = wr;
			ret = ENOMEM;
			goto bad_wr;
		}

		memset(&db, 0, sizeof(db));
		len = 0;

		hdr = (struct bnxt_re_bsqe *)db.wqe;
		hdr_v3 = (struct bnxt_re_bsqe_v3 *)hdr;
		lhdr_v3 = &hdr_v3->lhdr_v3;

		sqe = (struct bnxt_re_send *)&db.wqe[BNXT_RE_EXP_SQE_OFFSET];
		if (bnxt_re_sq_has_compact_wqe(qp, wr->opcode))
			sge = (struct bnxt_re_sge *)&db.wqe[BNXT_RE_EXP_COMP_SQ_SGE_OFFSET];
		else
			sge = (struct bnxt_re_sge *)&db.wqe[BNXT_RE_EXP_SQ_SGE_OFFSET];

		if (wr->num_sge) {
			len = bnxt_re_build_exp_tx_sge(qp, sge, wr);
			if (unlikely(len < 0 || len > BNXT_RE_MAX_MSG_SIZE)) {
				ret = EINVAL;
				*bad = wr;
				goto err;
			}
		}

		hdr_v3->opaque = htole32((qp->qpid << 16) | (sq->tail & 0xffff));
		bnxt_re_set_hdr_flags(hdr, sq, wr, wqe_size, qp->cap.sqsig,
				      qp->qptyp, true, len);

		if (qp->qptyp == IBV_QPT_UD)
			len = bnxt_re_build_ud_sqe(wr, hdr, sqe);

		switch (wr->opcode) {
		case IBV_WR_SEND_WITH_IMM:
			/* HW is swapping the immediate data before sending
			 * it out on the wire. To workaround this, swap the
			 * imm_data value as sent by the application so that
			 * the value going out on the wire is in big-endian
			 * format.
			 */
			lhdr_v3->send.inv_key_immd = htole32(be32toh(wr->imm_data));
			break;
		case IBV_WR_SEND:
			break;
		case IBV_WR_RDMA_WRITE_WITH_IMM:
			lhdr_v3->rdma.immd = htole32(be32toh(wr->imm_data));
		case IBV_WR_RDMA_WRITE:
		case IBV_WR_RDMA_READ:
			rsqe = (struct bnxt_re_rdma *)sqe;
			rsqe->rva = htole64(wr->wr.rdma.remote_addr);
			rsqe->rkey = htole32(wr->wr.rdma.rkey);
			break;
#ifdef HAVE_DRIVER1
		case IBV_WR_DRIVER1:
			if (BNXT_RE_UDP_SP_WQE(qp->cntx) &&
			    (qp->qptyp == IBV_QPT_RC ||
			     qp->qptyp == IBV_QPT_UD))
				lhdr_v3->udp.udp_port = htole16((uint16_t)wr->imm_data);
			else
				len = -EINVAL;
			break;
#endif
		default:
			len = -EINVAL;
			break;
		}

err:
		if (unlikely(len < 0)) {
			ret = (len == -EINVAL) ? EINVAL : ENOMEM;
			*bad = wr;
			break;
		}

		wrid = bnxt_re_get_swqe(qp->jsqq, &swq_idx);
		wrid->wrid = wr->wr_id;
		wrid->bytes = len;
		wrid->slots = slots;
		wrid->sig = (wr->send_flags & IBV_SEND_SIGNALED || qp->cap.sqsig) ?
			IBV_SEND_SIGNALED : 0;
		wrid->wc_opcd = ibv_wr_to_wc_opcd[wr->opcode];

		bnxt_re_incr_tail(sq, slots);
		bnxt_re_jqq_mod_start(qp->jsqq, swq_idx, 0);

		bnxt_re_ring_exp_db(qp, &db, true);

#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
		bnxt_dbg_dump_wr(wr, qp, hdr, sqe);
#endif
		qp->wqe_cnt++;
		qp->sq_msn++;
		wr = wr->next;
	}

bad_wr:
	bnxt_re_dp_spin_unlock(&sq->qlock);
	return ret;
}

int bnxt_re_post_send(struct ibv_qp *ibvqp, struct ibv_send_wr *wr,
		      struct ibv_send_wr **bad)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct bnxt_re_queue *sq = qp->jsqq->hwque;
	struct bnxt_re_push_buffer *pbuf = NULL;
	bool is_hsi_v3 = qp->cctx->is_hsi_v3;
	struct bnxt_re_bsqe_v3 *hdr_v3;
	union lower_shdr_v3 *lhdr_v3;
	int slots, ret = 0, len = 0;
	uint32_t swq_idx, wqe_size;
	struct bnxt_re_wrid *wrid;
	struct bnxt_re_rdma *rsqe;
	struct bnxt_re_bsqe *hdr;
	struct bnxt_re_send *sqe;
	unsigned int ilsize = 0;
	bool ring_db = false;
	int add_wrs = 0;
	uint32_t idx;

	if (unlikely(qp->exp_mode))
		return post_send_exp(ibvqp, wr, bad);

	if (qp->scq->ts_cq)
		return ibv_cmd_post_send(ibvqp, wr, bad);

	bnxt_re_dp_spin_lock(&sq->qlock);
	while (wr) {
		slots = bnxt_re_required_slots(qp, wr, &wqe_size,
					       (void **)&pbuf, &ilsize);
		if (unlikely(slots < 0 || bnxt_re_is_que_full(sq, slots)) ||
		    wr->num_sge > qp->cap.max_ssge) {
			*bad = wr;
			ret = ENOMEM;
			goto bad_wr;
		}
		idx = 1;
		len = 0;
		hdr = bnxt_re_get_hwqe(sq, 0);
		hdr_v3 = (struct bnxt_re_bsqe_v3 *)hdr;
		lhdr_v3 = &hdr_v3->lhdr_v3;

		sqe = bnxt_re_get_hwqe(sq, idx);
		if (!bnxt_re_sq_has_compact_wqe(qp, wr->opcode))
			idx++;

		/* populate push buffer */
		if (pbuf) {
			pbuf->qpid = qp->qpid;
			pbuf->wqe[0] = (__u64)hdr;
			pbuf->wqe[1] = (__u64)sqe;
			pbuf->st_idx = *sq->dbtail;
		}
		/* 'idx' here should be pointing to the first SGE */
		if (wr->num_sge) {
			len = bnxt_re_build_tx_sge(qp, sq, &idx, pbuf, wr, &ilsize, &slots);
			if (unlikely(len > BNXT_RE_MAX_MSG_SIZE)) {
				ret = EINVAL;
				*bad = wr;
				goto bad_wr;
			}
		}
		if (is_hsi_v3)
			hdr_v3->opaque = htole32((qp->qpid << 16) | (sq->tail & 0xffff));
		else
			hdr->lhdr.qkey_len = htole32(len);
		bnxt_re_set_hdr_flags(hdr, sq, wr, wqe_size, qp->cap.sqsig,
				      qp->qptyp, is_hsi_v3, len);
		switch (wr->opcode) {
		case IBV_WR_SEND_WITH_IMM:
			/* HW is swapping the immediate data before
			 * sending it out on the wire. To workaround
			 * this, swap the imm_data value as sent by
			 * the application so that the value going out
			 * on the wire is in big-endian format.
			 */
			if (is_hsi_v3)
				lhdr_v3->send.inv_key_immd =
					htole32(be32toh(wr->imm_data));
			else
				hdr->key_immd = htole32(be32toh(wr->imm_data));

			if (qp->qptyp == IBV_QPT_UD) {
				if (!qp->cctx->chip_is_p5_plus &&
				    qp->wqe_cnt == BNXT_RE_UD_QP_STALL)
					bnxt_re_force_rts2rts(qp);

				len = bnxt_re_build_ud_sqe(wr, hdr, sqe);
			}
			break;
#ifdef HAVE_SEND_WITH_INV
		case IBV_WR_SEND_WITH_INV:
			hdr->key_immd = htole32(wr->imm_data);

			if (is_hsi_v3) {
				fprintf(stderr,
					"IBV_WR_SEND_WITH_INV UD not supported on V3\n");
				len = -EINVAL;
				goto err;
			}
			/* fallthrough */
#endif
		case IBV_WR_SEND:
			if (qp->qptyp == IBV_QPT_UD) {
				if (unlikely(!qp->cctx->chip_is_p5_plus &&
					     qp->wqe_cnt == BNXT_RE_UD_QP_STALL))
					bnxt_re_force_rts2rts(qp);

				len = bnxt_re_build_ud_sqe(wr, hdr, sqe);
			}
			break;
		case IBV_WR_RDMA_WRITE_WITH_IMM:
			if (is_hsi_v3)
				lhdr_v3->rdma.immd = htole32(be32toh(wr->imm_data));
			else
				hdr->key_immd = htole32(be32toh(wr->imm_data));
		case IBV_WR_RDMA_WRITE:
		case IBV_WR_RDMA_READ:
			/* v3 has the same field locations */
			rsqe = (struct bnxt_re_rdma *)sqe;
			rsqe->rva = htole64(wr->wr.rdma.remote_addr);
			rsqe->rkey = htole32(wr->wr.rdma.rkey);
			break;
		case IBV_WR_ATOMIC_CMP_AND_SWP:
		case IBV_WR_ATOMIC_FETCH_AND_ADD:
			if (is_hsi_v3) {
				fprintf(stderr,
					"atomic operation not supported on V3\n");
				len = -EINVAL;
				goto err;
			}

			if (bnxt_re_build_atomic_sqe(qp, wr, hdr, sqe)) {
				ret = EINVAL;
				*bad = wr;
				goto bad_wr;
			}
			break;
#ifdef HAVE_WR_BIND_MW
		case IBV_WR_BIND_MW:
			len = bnxt_re_build_bind_sqe(qp, wr, hdr, sqe);
			break;
#endif
#ifdef HAVE_LOCAL_INV
		case IBV_WR_LOCAL_INV:
			len = bnxt_re_build_inval_sqe(qp, wr, hdr, sqe);
			break;
#endif
#ifdef HAVE_DRIVER1
		case IBV_WR_DRIVER1:
			if (BNXT_RE_UDP_SP_WQE(qp->cntx) &&
			    (qp->qptyp == IBV_QPT_RC || qp->qptyp == IBV_QPT_UD))
				lhdr_v3->udp.udp_port = htole16((uint16_t)wr->imm_data);
			else
				len = -EINVAL;
			break;
#endif
		default :
			len = -EINVAL;
			break;
		}

err:
		if (unlikely(len < 0)) {
			ret = (len == -EINVAL) ? EINVAL : ENOMEM;
			*bad = wr;
			break;
		}
		if (BNXT_RE_MSN_TBL_EN(qp->cntx) && !BNXT_RE_IQM(qp->cntx))
			bnxt_re_fill_psns_for_msntbl(qp, len, *sq->dbtail, wr->opcode);
		else
			bnxt_re_fill_psns(qp, len, *sq->dbtail, wr->opcode);

		wrid = bnxt_re_get_swqe(qp->jsqq, &swq_idx);
		wrid->wrid = wr->wr_id;
		wrid->bytes = len;
		wrid->slots = slots;
		wrid->sig = (wr->send_flags & IBV_SEND_SIGNALED || qp->cap.sqsig) ?
			     IBV_SEND_SIGNALED : 0;
		wrid->wc_opcd = ibv_wr_to_wc_opcd[wr->opcode];

		bnxt_re_incr_tail(sq, slots);
		bnxt_re_jqq_mod_start(qp->jsqq, swq_idx, 0);

		if ((wr->send_flags & IBV_SEND_INLINE) && qp->inline_opt_enabled) {
			add_wrs = ((slots - wqe_size) >> 1);
			if (add_wrs) {
				wrid = bnxt_re_get_swqe(qp->jsqq, &swq_idx);
				wrid->slots = 0;
				wrid->pad_wqe_count = add_wrs;
				bnxt_re_jqq_mod_start(qp->jsqq, swq_idx, (add_wrs - 1));
			}
		}
		ring_db = true;
		if (pbuf) {
#ifdef BNXT_RE_ENABLE_DEFFER_DB
			if (qp->cntx && qp->cntx->deferred_db_enabled) {
			/* gonna do push so clear all other pending DBs */
				if (unlikely(qp->sq_db_ctrl.defered))
					qp->sq_db_ctrl.force = true;
				bnxt_re_db_ctrl_check(&qp->sq_db_ctrl);
			}
#endif
			ring_db = false;
			pbuf->tail = *sq->dbtail;
			if (qp->cctx->chip_is_gen_p7) {
				bnxt_re_fill_ppp(pbuf, qp, len, idx);
			} else {
				bnxt_re_fill_push_wcb(qp, pbuf, idx, ilsize);
			}
			pbuf = NULL;
			qp->sq_db_ctrl.last_tm = timemark_ns();
		}
#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
		bnxt_dbg_dump_wr(wr, qp, hdr, sqe);
#endif
		qp->wqe_cnt++;
		qp->sq_msn++;
		wr = wr->next;
	}

bad_wr:
	if (ring_db) {
		qp->sq_db_ctrl.last_tm = timemark_ns();
#ifdef BNXT_RE_ENABLE_DEFFER_DB
		if (qp->cntx && qp->cntx->deferred_db_enabled)
			bnxt_re_db_ctrl_update(&qp->sq_db_ctrl, qp);
#else
		bnxt_re_ring_sq_db(qp);
#endif
	}

	bnxt_re_dp_spin_unlock(&sq->qlock);
	return ret;
}

int bnxt_re_post_recv(struct ibv_qp *ibvqp, struct ibv_recv_wr *wr,
		      struct ibv_recv_wr **bad)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct bnxt_re_queue *rq = qp->jrqq->hwque;
	struct bnxt_re_wrid *swque;
	struct bnxt_re_exp_db db;
	struct bnxt_re_brqe *hdr;
	struct bnxt_re_sge *sge;
	bool ring_db = false;
	uint32_t swq_idx;
	uint32_t hdrval;
	uint32_t idx;
	int rc = 0;

	if (qp->rcq->ts_cq) {
		memset((void *)wr->sg_list[0].addr, 0, wr->sg_list[0].length);
		return ibv_cmd_post_recv(ibvqp, wr, bad);
	}

	bnxt_re_dp_spin_lock(&rq->qlock);
	while (wr) {
		if (unlikely(bnxt_re_is_que_full(rq, rq->max_slots) ||
			     wr->num_sge > qp->cap.max_rsge)) {
			*bad = wr;
			rc = ENOMEM;
			break;
		}
		swque = bnxt_re_get_swqe(qp->jrqq, &swq_idx);

		if (qp->exp_mode)
			memset(&db, 0, sizeof(db));

		/*
		 * Initialize idx to 2 since the length of header wqe is 32 bytes
		 * i.e. sizeof(struct bnxt_re_brqe) + sizeof(struct bnxt_re_send)
		 *
		 * For V3, length of header is 16 bytes, so idx should be
		 * initialized to 1
		 */
		idx = qp->cctx->is_hsi_v3 ? 1 : 2;
		hdr = qp->exp_mode ? &db.wqe[0] : bnxt_re_get_hwqe_hdr(rq);

		if (unlikely(!wr->num_sge)) {
			/*
			 * HW needs at least one SGE for RQ Entries.
			 * Create an entry if num_sge = 0,
			 * update the idx and set length of sge to 0.
			 */
			sge = qp->exp_mode ? &db.wqe[0] : bnxt_re_get_hwqe(rq, idx);
			idx++;
			sge->length = 0;
		} else {
			/* Fill SGEs */
			bnxt_re_put_rx_sge(qp, &db, rq, &idx, wr->sg_list,
					   wr->num_sge);
		}
		hdrval = qp->cctx->is_hsi_v3 ?
			 BNXT_RE_WR_OPCD_RECV_V3 : BNXT_RE_WR_OPCD_RECV;
#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
		hdrval = bnxt_re_rq_hdr_val_test(rq, hdrval);
#endif
		hdrval |= ((idx & BNXT_RE_HDR_WS_MASK) << BNXT_RE_HDR_WS_SHIFT);
		hdr->rsv_ws_fl_wt = htole32(hdrval);
		if (qp->cctx->is_hsi_v3)
			/*
			 * Encode QP ID to bits: 31:20 (12 bits)
			 * Encode swq_idx to bits: 19:0 (20 bits)
			 */
			hdr->opaque = htole32((qp->qpid << 20) |
					      (swq_idx & BNXT_RE_BCQE_RWRID_MASK));
		else
			hdr->wrid = htole32(swq_idx);

		swque->wrid = wr->wr_id;
		swque->slots = rq->max_slots;
		swque->wc_opcd = qp->cctx->is_hsi_v3 ?
			BNXT_RE_WC_OPCD_RECV_V3 : BNXT_RE_WC_OPCD_RECV;

		bnxt_re_jqq_mod_start(qp->jrqq, swq_idx, 0);
		bnxt_re_incr_tail(rq, rq->max_slots);

		if (qp->exp_mode) {
			bnxt_re_ring_exp_db(qp, &db, false);
			ring_db = false;
		} else {
			ring_db = true;
		}

		wr = wr->next;
	}
	if (ring_db) {
		if (unlikely(qp->qpst != IBV_QPS_INIT))
#ifdef BNXT_RE_ENABLE_DEFFER_DB
			if (qp->cntx && qp->cntx->deferred_db_enabled)
				bnxt_re_db_ctrl_update(&qp->rq_db_ctrl, qp);
#else
			bnxt_re_ring_rq_db(qp);
#endif
	}
	bnxt_re_dp_spin_unlock(&rq->qlock);

	return rc;
}

static size_t bnxt_re_get_srqmem_size(struct bnxt_re_context *cntx,
				      struct ibv_srq_init_attr *attr,
				      struct bnxt_re_qattr *qattr)
{
	uint32_t stride, nswr;
	size_t size = 0, nsge;

	size = sizeof(struct bnxt_re_srq);
	size += sizeof(struct bnxt_re_queue);
	/* allocate 1 extra to determin full condition */
	nswr = attr->attr.max_wr + 1;
	nswr = bnxt_re_init_depth(nswr, cntx->comp_mask);
	if (cntx->rdev->small_rx_wqe)
		nsge = attr->attr.max_sge;
	else
		nsge = BNXT_RE_STATIC_WQE_MAX_SGE;

	stride = bnxt_re_get_srqe_sz(cntx->cctx, nsge);

	qattr->nwr = nswr;
	qattr->slots = nswr;
	qattr->esize = stride;

	qattr->sz_ring = get_aligned((nswr * stride), cntx->rdev->pg_size);
	qattr->sz_shad = nswr * sizeof(struct bnxt_re_wrid); /* shadow */

	size += qattr->sz_ring;
	size += qattr->sz_shad;
	return size;
}

static void *bnxt_re_alloc_srqslab(struct bnxt_re_context *cntx,
				   struct ibv_srq_init_attr *attr,
				   struct bnxt_re_qattr *qattr,
				   struct ibv_pd *ibvpd)
{
	struct bnxt_re_parent_domain *pa_dom;
	size_t bytes;

	bytes = bnxt_re_get_srqmem_size(cntx, attr, qattr);
	pa_dom = bnxt_re_get_parent_domain(ibvpd);
	return bnxt_re_alloc_mem(bytes, cntx->rdev->pg_size, pa_dom);
}

static struct bnxt_re_srq *bnxt_re_srq_alloc_queue_ptr(struct bnxt_re_mem *mem)
{
	struct bnxt_re_srq *srq;

	srq = bnxt_re_get_obj(mem, sizeof(*srq));
	if (!srq)
		return NULL;
	srq->srqq = bnxt_re_get_obj(mem, sizeof(struct bnxt_re_queue));
	if (!srq->srqq)
		return NULL;
	return srq;
}

static int bnxt_re_srq_alloc_queue(struct bnxt_re_srq *srq,
				   struct ibv_srq_init_attr *attr,
				   struct bnxt_re_qattr *qattr,
				   int need_lock)
{
	struct bnxt_re_queue *que;
	int ret = -ENOMEM;
	int idx;

	que = srq->srqq;
	que->depth = qattr->slots;
	que->stride = qattr->esize;
	que->va = bnxt_re_get_ring(srq->mem, qattr->sz_ring);
	if (!que->va)
		goto bail;
	bnxt_re_dp_spin_init(&que->qlock, PTHREAD_PROCESS_PRIVATE, need_lock);
	/* For SRQ only bnxt_re_wrid.wrid is used. */
	srq->srwrid = bnxt_re_get_obj(srq->mem, qattr->sz_shad);
	if (!srq->srwrid)
		goto bail;

	srq->start_idx = 0;
	srq->last_idx = que->depth - 1;
	for (idx = 0; idx < que->depth; idx++)
		srq->srwrid[idx].next_idx = idx + 1;
	srq->srwrid[srq->last_idx].next_idx = -1;
	/*TODO: update actual max depth. */
	return 0;
bail:
	bnxt_re_dp_spin_destroy(&srq->srqq->qlock);
	return ret;
}

struct ibv_srq *bnxt_re_create_srq(struct ibv_pd *ibvpd,
				   struct ibv_srq_init_attr *attr)
{
	struct bnxt_re_srq_resp resp = {};
	struct bnxt_re_srq_req cmd = {};
	struct bnxt_re_qattr qattr = {};
	struct bnxt_re_context *uctx;
	struct ibv_context *ibvctx;
	struct bnxt_re_srq *srq;
	struct bnxt_re_parent_domain *pa_dom;
	void *mem;
	int need_lock;
	int ret;

	uctx = to_bnxt_re_context(ibvpd->context);

	ibvctx = ibvpd->context;
	pa_dom = bnxt_re_get_parent_domain(ibvpd);
	need_lock = !bnxt_single_threaded && (!pa_dom || !pa_dom->td);
	/*TODO: Check max limit on queue depth and sge.*/
	mem = bnxt_re_alloc_srqslab(uctx, attr, &qattr, ibvpd);
	if (!mem)
		return NULL;

	srq = bnxt_re_srq_alloc_queue_ptr(mem);
	if (!srq)
		goto fail;
	srq->uctx = uctx;
	srq->mem = mem;
	if (bnxt_re_srq_alloc_queue(srq, attr, &qattr, need_lock))
		goto fail;

	cmd.srqva = (uint64_t)srq->srqq->va;
	cmd.srq_handle = (uint64_t)srq;
	cmd.srqprodva = (uint64_t)&srq->srqq->tail;
	cmd.srqconsva = (uint64_t)&srq->srqq->head;
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	cmd.comp_mask |= BNXT_RE_QP_REQ_MASK_DUMP_SRQ_INDEX;
	ret = ibv_cmd_create_srq(ibvpd, &srq->ibvsrq, attr,
				 &cmd.cmd, sizeof(cmd),
				 &resp.resp, sizeof(resp));
	if (ret)
		goto fail;

	srq->srqid = resp.srqid;
	if (resp.comp_mask & BNXT_RE_SRQ_HDBR_MMAP_KEY)
		srq->dbc = bnxt_re_hdbr_map_dbc(ibvpd->context, resp.hdbr_srq_mmap_key);

	if (resp.comp_mask & BNXT_RE_SRQ_TOGGLE_PAGE_SUPPORT) {
		struct bnxt_re_mmap_info minfo = {};
		int ret;

		minfo.type = BNXT_RE_SRQ_TOGGLE_MEM;
		minfo.res_id = resp.srqid;
		ret = bnxt_re_get_toggle_mem(ibvctx, &minfo, NULL);
		if (ret)
			goto fail;
		srq->toggle_map = mmap(NULL, minfo.alloc_size, PROT_READ,
				       MAP_SHARED, ibvctx->cmd_fd,
				       minfo.alloc_offset);
		if (srq->toggle_map == MAP_FAILED)
			goto fail;

		srq->toggle_size = minfo.alloc_size;
	}

	srq->udpi = &uctx->udpi;
	srq->cap.max_wr = srq->srqq->depth;
	srq->cap.max_sge = attr->attr.max_sge;
	srq->cap.srq_limit = attr->attr.srq_limit;
	srq->rand.seed = srq->srqid;
	srq->shadow_db_key = BNXT_RE_DB_KEY_INVALID;

	INIT_DBLY_LIST_NODE(&srq->dbnode);
	if (pa_dom) {
		srq->parent_domain = pa_dom;
		atomic_fetch_add(&pa_dom->refcount, 1);
	} else {
		srq->parent_domain = NULL;
	}
	return &srq->ibvsrq;
fail:
	bnxt_re_free_mem(mem, pa_dom);
	return NULL;
}

int bnxt_re_modify_srq(struct ibv_srq *ibvsrq, struct ibv_srq_attr *attr,
		       int attr_mask)
{
	struct ibv_modify_srq cmd = {};

	return ibv_cmd_modify_srq(ibvsrq, attr, attr_mask,
				     &cmd, sizeof(cmd));
}

int bnxt_re_destroy_srq(struct ibv_srq *ibvsrq)
{
	struct bnxt_re_srq *srq = to_bnxt_re_srq(ibvsrq);
	struct bnxt_re_mem *mem;
	int ret;

	if (srq->toggle_map)
		munmap(srq->toggle_map, srq->toggle_size);

	bnxt_re_hdbr_unmap_dbc(srq->dbc);
	ret = ibv_cmd_destroy_srq(ibvsrq);
	if (ret)
		return ret;
	if (srq->parent_domain)
		atomic_fetch_sub(&srq->parent_domain->refcount, 1);
	bnxt_re_dp_spin_destroy(&srq->srqq->qlock);
	mem = srq->mem;
	bnxt_re_free_mem(mem, srq->parent_domain);
	return 0;
}

int bnxt_re_query_srq(struct ibv_srq *ibvsrq, struct ibv_srq_attr *attr)
{
	struct ibv_query_srq cmd = {};

	return ibv_cmd_query_srq(ibvsrq, attr, &cmd, sizeof cmd);
}

static void bnxt_re_build_srqe(struct bnxt_re_srq *srq,
			       struct ibv_recv_wr *wr, void *srqe)
{
	struct bnxt_re_brqe *hdr = srqe;
	struct bnxt_re_context *uctx;
	struct bnxt_re_wrid *wrid;
	struct bnxt_re_sge *sge;
	int wqe_sz, len, next;
	uint32_t hdrval = 0;
	int indx;

	uctx = srq->uctx;
	sge = (srqe + bnxt_re_get_srqe_hdr_sz(uctx->cctx));
	next = srq->start_idx;
	wrid = &srq->srwrid[next];

	len = 0;
	for (indx = 0; indx < wr->num_sge; indx++, sge++) {
		sge->pa = htole64(wr->sg_list[indx].addr);
		sge->lkey = htole32(wr->sg_list[indx].lkey);
		sge->length = htole32(wr->sg_list[indx].length);
		len += wr->sg_list[indx].length;
	}

	hdrval = uctx->cctx->is_hsi_v3 ?
		 BNXT_RE_WR_OPCD_RECV_V3 : BNXT_RE_WR_OPCD_RECV;

	wqe_sz = wr->num_sge + (bnxt_re_get_srqe_hdr_sz(uctx->cctx) >> 4); /* 16B align */
	/* HW needs at least one SGE for SRQ Entries.
	 * Increment SRQ WQE size if num_sge = 0 to
	 * include the extra SGE. Set the sge length to
	 * zero.
	 */
	if (!wr->num_sge) {
		wqe_sz++;
		sge->length = 0;
	}
	hdrval |= ((wqe_sz & BNXT_RE_HDR_WS_MASK) << BNXT_RE_HDR_WS_SHIFT);
	hdr->rsv_ws_fl_wt = htole32(hdrval);
	/* V3 uses opaque instead of wrid */
	if (uctx->cctx->is_hsi_v3) {
		hdr->opaque = htole32((srq->srqid << 20) |
				      (next & BNXT_RE_BCQE_RWRID_MASK));
	} else {
		hdr->wrid = htole32((uint32_t)next);
	}

	/* Fill wrid */
	wrid->wrid = wr->wr_id;
	wrid->bytes = len; /* N.A. for RQE */
	wrid->sig = 0; /* N.A. for RQE */
}

int bnxt_re_post_srq_recv(struct ibv_srq *ibvsrq, struct ibv_recv_wr *wr,
			  struct ibv_recv_wr **bad)
{
	struct bnxt_re_srq *srq = to_bnxt_re_srq(ibvsrq);
	struct bnxt_re_queue *rq = srq->srqq;
	struct bnxt_re_context *uctx;
	int count = 0, rc = 0;
	bool ring_db = false;
	void *srqe;

	bnxt_re_dp_spin_lock(&rq->qlock);
	uctx = to_bnxt_re_context(ibvsrq->context);
#ifdef BNXT_RE_ENABLE_DEFFER_DB
	if (srq->uctx && srq->uctx->deferred_db_enabled)
		bnxt_re_db_ctrl_poll();
#endif
	count = rq->tail > rq->head ? rq->tail - rq->head :
			   rq->depth - rq->head + rq->tail;
	while (wr) {
		if (srq->start_idx == srq->last_idx ||
		    wr->num_sge > srq->cap.max_sge) {
			*bad = wr;
			rc = ENOMEM;
			goto exit;
		}

		srqe = (void *) (rq->va + (rq->tail * rq->stride));
		memset(srqe, 0, bnxt_re_get_srqe_sz(uctx->cctx, srq->cap.max_sge));
		bnxt_re_build_srqe(srq, wr, srqe);

		srq->start_idx = srq->srwrid[srq->start_idx].next_idx;
		bnxt_re_incr_tail(rq, 1);
		ring_db = true;
		wr = wr->next;
		count++;
	}
exit:
	if (ring_db)
		bnxt_re_ring_srq_db(srq);

	bnxt_re_dp_spin_unlock(&rq->qlock);

	return rc;
}

struct ibv_ah *bnxt_re_create_ah(struct ibv_pd *ibvpd, struct ibv_ah_attr *attr)
{
	struct bnxt_re_context *uctx;
	struct bnxt_re_ah_resp resp;
	struct bnxt_re_pd *pd;
	struct bnxt_re_ah *ah;
	int status;

	pd = to_bnxt_re_pd(ibvpd);
	uctx = to_bnxt_re_context(ibvpd->context);

	ah = calloc(1, sizeof(struct bnxt_re_ah));
	if (!ah) {
		goto failed;
	}

	resp.ah_id = 0;
	ah->pd = pd;
	pthread_mutex_lock(&uctx->shlock);
	status = ibv_cmd_create_ah(ibvpd, &ah->ibvah, attr,
				   &resp.resp, sizeof(resp));

	if (status)
	{
		pthread_mutex_unlock(&uctx->shlock);
		free(ah);
		goto failed;
	}
	/* read AV ID now. */
	ah->avid = resp.ah_id;
	pthread_mutex_unlock(&uctx->shlock);

	return &ah->ibvah;
failed:
	return NULL;
}

int bnxt_re_destroy_ah(struct ibv_ah *ibvah)
{
	struct bnxt_re_ah *ah;
	int status;

	ah = to_bnxt_re_ah(ibvah);
	status = ibv_cmd_destroy_ah(ibvah);
	if (status)
		return status;
	free(ah);

	return 0;
}

struct ibv_flow *bnxt_re_create_flow(struct ibv_qp *qp, struct ibv_flow_attr *attr)
{
	struct ibv_flow *flow;
	int ret;

	flow = calloc(1, sizeof(*flow));
	if (!flow)
		return NULL;

	ret = ibv_cmd_create_flow_compat(qp, flow, attr);
	if (ret) {
		free(flow);
		flow = NULL;
	}

	return flow;
}

int bnxt_re_destroy_flow(struct ibv_flow *flow)
{
	int ret;

	ret = ibv_cmd_destroy_flow(flow);

	free(flow);
	return ret;
}

#ifdef HAVE_WR_BIND_MW
struct ibv_mw *bnxt_re_alloc_mw(struct ibv_pd *ibvpd, enum ibv_mw_type type)
{
	struct ibv_alloc_mw cmd = {};
	struct ibv_mw *ibvmw;
	int status;
#ifdef RCP_USE_IB_UVERBS
	struct ib_uverbs_alloc_mw_resp resp = {};
#else
	struct ibv_alloc_mw_resp resp = {};
#endif

	ibvmw = calloc(1, sizeof(*ibvmw));
	if (!ibvmw)
		return NULL;

	status = ibv_cmd_alloc_mw(ibvpd, type, ibvmw, &cmd,
				  sizeof(cmd), &resp, sizeof(resp));
	if (status) {
		free(ibvmw);
		return NULL;
	}

	return ibvmw;
}

int bnxt_re_dealloc_mw(struct ibv_mw *ibvmw)
{
#ifndef IBV_CMD_ALLOC_MW_HAS_1_ARG
	struct ibv_dealloc_mw cmd = {};
#endif
	int status;

#ifndef IBV_CMD_ALLOC_MW_HAS_1_ARG
	status = ibv_cmd_dealloc_mw(ibvmw, &cmd, sizeof(cmd));
#else
	status = ibv_cmd_dealloc_mw(ibvmw);
#endif
	if (status)
		return status;

	free(ibvmw);

	return 0;
}

int bnxt_re_bind_mw(struct ibv_qp *ibvqp, struct ibv_mw *ibvmw,
		    struct ibv_mw_bind *bind)
{
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_send_wr wr = {};
	int status;

	wr.opcode = IBV_WR_BIND_MW;
	wr.next = NULL;

	wr.wr_id = bind->wr_id;
	wr.send_flags = bind->send_flags;

	wr.bind_mw.mw = ibvmw;
	wr.bind_mw.rkey = ibv_inc_rkey(ibvmw->rkey);
	wr.bind_mw.bind_info = bind->bind_info;

	status = bnxt_re_post_send(ibvqp, &wr, &bad_wr);
	if (status)
		return status;
	/* Update the rkey */
	ibvmw->rkey = wr.bind_mw.rkey;

	return 0;
}

#ifdef HAVE_ECE_OPTIONS
int bnxt_re_set_ece(struct ibv_qp *ibqp, struct ibv_ece *ece)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibqp);

	if (ece->comp_mask)
		return -EINVAL;

	if (ece->vendor_id != PCI_VENDOR_ID_BROADCOM)
		return -EINVAL;

	qp->set_ece = ece->options;
	/* Clean previously returned ECE options */
	qp->get_ece = 0;
	bnxt_trace_config(qp->cntx, "%s ece vendorid x%02x options %d\n",
			  __func__, ece->vendor_id, ece->options);
	return 0;
}

int bnxt_re_query_ece(struct ibv_qp *ibqp, struct ibv_ece *ece)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibqp);

	ece->vendor_id = PCI_VENDOR_ID_BROADCOM;
	ece->options = qp->get_ece;
	ece->comp_mask = 0;
	bnxt_trace_config(qp->cntx, "%s ece vendorid x%02x options %d\n",
			  __func__, ece->vendor_id, ece->options);
	return 0;
}
#endif

#endif
