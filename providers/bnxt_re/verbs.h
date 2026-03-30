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

#ifndef __BNXT_RE_VERBS_H__
#define __BNXT_RE_VERBS_H__

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

#ifdef HAVE_RDMA_CORE_PKG
#include "driver.h"
#else
#include <infiniband/driver.h>
#include <infiniband/arch.h>
#endif

#include <infiniband/verbs.h>

#define BNXT_RE_MAX_MSG_SIZE	0x80000000

/* Default MTU set for QPs during create_qp */
#define BNXT_RE_QP_MTU_DEFAULT  1024

#ifdef VERBS_ONLY_QUERY_DEVICE_EX_DEFINED
#define offsetofend(_type, _member) \
	(offsetof(_type, _member) + sizeof(((_type *)0)->_member))

int bnxt_re_query_device_ex(struct ibv_context *ibvctx,
			    const struct ibv_query_device_ex_input *input,
			    struct ibv_device_attr_ex *attr, size_t attr_size);
#else
int bnxt_re_query_device(struct ibv_context *ibvctx,
			 struct ibv_device_attr *dev_attr);
#endif

int bnxt_re_query_device_compat(struct ibv_context *ibvctx,
				struct ibv_device_attr *dev_attr);

int bnxt_re_query_port(struct ibv_context *, uint8_t, struct ibv_port_attr *);

struct ibv_pd *bnxt_re_alloc_pd(struct ibv_context *);
int bnxt_re_free_pd(struct ibv_pd *);

#ifndef VERBS_MR_DEFINED
typedef struct ibv_mr VERBS_MR;
#else
typedef struct verbs_mr VERBS_MR;
#endif

struct ibv_mr *bnxt_re_reg_mr(struct ibv_pd *, void *, size_t,
#ifdef REG_MR_VERB_HAS_5_ARG
			      uint64_t,
#endif
			      int ibv_access_flags);
int bnxt_re_dereg_mr(VERBS_MR*);

#ifdef HAVE_IBV_DMABUF
struct ibv_mr *bnxt_re_reg_dmabuf_mr(struct ibv_pd *, uint64_t,
				     size_t, uint64_t, int, int);
#endif

struct ibv_cq *bnxt_re_create_cq(struct ibv_context *, int,
				 struct ibv_comp_channel *, int);
int bnxt_re_resize_cq(struct ibv_cq *, int);
int bnxt_re_destroy_cq(struct ibv_cq *);
int bnxt_re_poll_cq(struct ibv_cq *, int, struct ibv_wc *);
void bnxt_re_cq_event(struct ibv_cq *);
int bnxt_re_arm_cq(struct ibv_cq *, int);
struct ibv_cq_ex *bnxt_re_create_cq_ex(struct ibv_context *ibvctx,
				       struct ibv_cq_init_attr_ex *attr_ex);

struct ibv_qp *bnxt_re_create_qp(struct ibv_pd *, struct ibv_qp_init_attr *);
#ifdef HAVE_IBV_WR_API
struct ibv_qp *bnxt_re_create_qp_ex(struct ibv_context *cntx,
				    struct ibv_qp_init_attr_ex *attr);
#endif
int bnxt_re_modify_qp(struct ibv_qp *, struct ibv_qp_attr *,
		      int ibv_qp_attr_mask);
int bnxt_re_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		     int attr_mask, struct ibv_qp_init_attr *init_attr);
int bnxt_re_destroy_qp(struct ibv_qp *);
int bnxt_re_post_send(struct ibv_qp *, struct ibv_send_wr *,
		      struct ibv_send_wr **);
int bnxt_re_post_recv(struct ibv_qp *, struct ibv_recv_wr *,
		      struct ibv_recv_wr **);

struct ibv_srq *bnxt_re_create_srq(struct ibv_pd *,
				   struct ibv_srq_init_attr *);
int bnxt_re_modify_srq(struct ibv_srq *, struct ibv_srq_attr *, int);
int bnxt_re_destroy_srq(struct ibv_srq *);
int bnxt_re_query_srq(struct ibv_srq *ibsrq, struct ibv_srq_attr *attr);
int bnxt_re_post_srq_recv(struct ibv_srq *, struct ibv_recv_wr *,
			  struct ibv_recv_wr **);

struct ibv_ah *bnxt_re_create_ah(struct ibv_pd *, struct ibv_ah_attr *);
int bnxt_re_destroy_ah(struct ibv_ah *);

struct ibv_flow *bnxt_re_create_flow(struct ibv_qp *qp, struct ibv_flow_attr *flow);
int bnxt_re_destroy_flow(struct ibv_flow *flow);

#ifdef HAVE_WR_BIND_MW
struct ibv_mw *bnxt_re_alloc_mw(struct ibv_pd *ibv_pd, enum ibv_mw_type type);
int bnxt_re_dealloc_mw(struct ibv_mw *ibv_mw);
int bnxt_re_bind_mw(struct ibv_qp *ibv_qp, struct ibv_mw *ibv_mw,
		    struct ibv_mw_bind *ibv_bind);
#endif

int bnxt_re_attach_mcast(struct ibv_qp *, const union ibv_gid *, uint16_t);
int bnxt_re_detach_mcast(struct ibv_qp *, const union ibv_gid *, uint16_t);

#ifdef ASYNC_EVENT_VERB_HAS_1_ARG
void bnxt_re_async_event(struct ibv_async_event *event);
#else
void bnxt_re_async_event(struct ibv_context *context,
			 struct ibv_async_event *event);
#endif
#ifdef HAVE_ECE_OPTIONS
int bnxt_re_set_ece(struct ibv_qp *ibqp, struct ibv_ece *ece);
int bnxt_re_query_ece(struct ibv_qp *ibqp, struct ibv_ece *ece);
#endif

void *bnxt_re_alloc_cqslab(struct bnxt_re_context *cntx,
			   uint32_t ncqe, uint32_t cur);
int bnxt_re_check_qp_limits(struct bnxt_re_context *cntx,
			    struct ibv_qp_init_attr_ex *attr);
void *bnxt_re_alloc_qpslab(struct bnxt_re_context *cntx,
			   struct ibv_qp_init_attr_ex *attr,
			   struct bnxt_re_qattr *qattr);
int bnxt_re_alloc_queue_ptr(struct bnxt_re_qp *qp,
			    struct ibv_qp_init_attr_ex *attr);
int bnxt_re_alloc_queues(struct bnxt_re_qp *qp,
			 struct ibv_qp_init_attr_ex *attr,
			 struct bnxt_re_qattr *qattr);
void bnxt_re_cleanup_cq(struct bnxt_re_qp *qp,
			struct bnxt_re_cq *cq);
int bnxt_re_get_sqmem_size(struct bnxt_re_context *cntx,
			   struct ibv_qp_init_attr_ex *attr,
			   struct bnxt_re_qattr *qattr);
int bnxt_re_get_rqmem_size(struct bnxt_re_context *cntx,
			   struct ibv_qp_init_attr_ex *attr,
			   struct bnxt_re_qattr *qattr);

struct bnxt_re_work_compl{
	struct bnxt_re_list_node cnode;
	struct ibv_wc wc;
};

static inline uint8_t bnxt_re_get_psne_size(struct bnxt_re_context *cntx)
{
	return (BNXT_RE_MSN_TBL_EN(cntx)) ? sizeof(struct bnxt_re_msns) :
					      (cntx->cctx->chip_is_gen_p5_p7) ?
					      sizeof(struct bnxt_re_psns_ext) :
					      sizeof(struct bnxt_re_psns);
}

static inline uint32_t bnxt_re_get_npsn(uint8_t mode, uint32_t nwr,
					uint32_t slots)
{
	return mode == BNXT_RE_WQE_MODE_VARIABLE ? slots : nwr;
}

static inline bool bnxt_re_is_mqp_ex_supported(struct bnxt_re_context *cntx)
{
	return cntx->comp_mask & BNXT_RE_UCNTX_CMASK_MQP_EX_SUPPORTED;
}

static inline bool can_request_ppp(struct bnxt_re_qp *re_qp,
				   struct ibv_qp_attr *attr, int attr_mask)
{
	struct bnxt_re_context *cntx;
	struct bnxt_re_qp *qp;
	bool request = false;

	qp = re_qp;
	cntx = qp->cntx;
	if (!qp->push_st_en && cntx->udpi.wcdpi && (attr_mask & IBV_QP_STATE) &&
	    qp->qpst == IBV_QPS_RESET && attr->qp_state == IBV_QPS_INIT &&
	    qp->cap.max_inline) {
		pthread_mutex_lock(&cntx->shlock);
		if (cntx->ppp_cnt < BNXT_RE_PUSH_MAX_PPP_PER_CTX) {
			cntx->ppp_cnt++;
			request = true;
		}
		pthread_mutex_unlock(&cntx->shlock);
	}
	return request;
}

static inline uint64_t bnxt_re_update_msn_tbl(uint32_t st_idx, uint32_t npsn, uint32_t start_psn)
{
	/* Adjust the field values to their respective ofsets */
	return htole64((((uint64_t)(st_idx) << BNXT_RE_SQ_MSN_SEARCH_START_IDX_SHIFT) &
		 BNXT_RE_SQ_MSN_SEARCH_START_IDX_MASK) |
		 (((uint64_t)(npsn) << BNXT_RE_SQ_MSN_SEARCH_NEXT_PSN_SHIFT) &
		 BNXT_RE_SQ_MSN_SEARCH_NEXT_PSN_MASK) |
		 (((start_psn) << BNXT_RE_SQ_MSN_SEARCH_START_PSN_SHIFT) &
		 BNXT_RE_SQ_MSN_SEARCH_START_PSN_MASK));
}

static inline int bnxt_re_get_max_inline_size(struct bnxt_re_context *cntx)
{
	if (cntx->cctx->chip_is_p8)
		return BNXT_RE_MAX_WCB_SIZE_VAR_WQE_TH3C;
	else
		return BNXT_RE_MAX_WCB_SIZE_VAR_WQE;
}

#define BNXT_RE_MSN_IDX(m) (((m) & BNXT_RE_SQ_MSN_SEARCH_START_IDX_MASK) >> \
		BNXT_RE_SQ_MSN_SEARCH_START_IDX_SHIFT)
#define BNXT_RE_MSN_NPSN(m) (((m) & BNXT_RE_SQ_MSN_SEARCH_NEXT_PSN_MASK) >> \
		BNXT_RE_SQ_MSN_SEARCH_NEXT_PSN_SHIFT)
#define BNXT_RE_MSN_SPSN(m) (((m) & BNXT_RE_SQ_MSN_SEARCH_START_PSN_MASK) >> \
		BNXT_RE_SQ_MSN_SEARCH_START_PSN_SHIFT)
#endif /* __BNXT_RE_VERBS_H__ */
