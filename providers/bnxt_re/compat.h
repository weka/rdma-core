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
 * Description: Compatibility data structures/codes
 */

#ifndef __BNXT_RE_COMPAT_H__
#define __BNXT_RE_COMPAT_H__

/*
 * v47 rdma-core feature flags - these features are present in rdma-core v47
 * and we always compile against v47, so define them unconditionally.
 */
#define VERBS_MR_DEFINED        1  /* struct verbs_mr exists */
#define VERBS_CQ_DEFINED        1  /* struct verbs_cq exists */
#define RCP_USE_IB_UVERBS       1  /* use ib_uverbs structures */
#define HAVE_RDMA_CORE_PKG      1  /* building with rdma-core package */
#define HAVE_WR_BIND_MW         1  /* IBV_WR_BIND_MW exists */
#define HAVE_LOCAL_INV          1  /* IBV_WR_LOCAL_INV exists */
#define HAVE_SEND_WITH_INV      1  /* IBV_WR_SEND_WITH_INV exists */
#define HAVE_IBV_WR_API         1  /* new WR API exists */
#define HAVE_IBV_DMABUF         1  /* dmabuf MR support */
#define HAVE_ECE_OPTIONS        1  /* ECE options */
#define VERBS_ONLY_QUERY_DEVICE_EX_DEFINED  1  /* only query_device_ex, not query_device */
#define IBV_FREE_CONTEXT_IN_CONTEXT_OPS  1  /* free_context in context ops */
#define HAVE_IBV_CMD_MODIFY_QP_EX  1  /* ibv_cmd_modify_qp_ex exists */
#define IBV_CMD_CREATE_QP_EX_HAS_7_ARG  1  /* 7-arg version */
#define IBV_CMD_CREATE_CQ_EX_HAS_8_ARG  1  /* 8-arg version */
/* IBV_CMD_REG_DMABUF_MR: v47 has 7 args (no driver arg) */
#define IBV_CMD_MODIFY_QP_EX_HAS_7_ARG  1  /* 7-arg version */
#define IBV_CMD_CREATE_FLOW_HAS_5_ARG   1  /* 5-arg version */
#define IBV_CMD_CREATE_CQ_EX_HAS_CONST_CQ_INIT_ATTR  1  /* const attr */
#define HAVE_IBV_FD_ARRAY       0  /* no ibv_fd_arr (v47 doesn't have it) */
#define SCHED_YIELD_DEFINED     1  /* use sched_yield */
#define HAVE_DRIVER1            1  /* driver1 opcode */
#define REG_MR_VERB_HAS_5_ARG   1  /* ibv_cmd_reg_mr has hca_va (5th arg) */
#define IBV_CMD_ALLOC_MW_HAS_1_ARG  1  /* ibv_cmd_dealloc_mw takes only mw arg */

/* offsetofend: not in user-space stddef.h but used in 238 provider code */
#ifndef offsetofend
#include <stddef.h>
#define offsetofend(type, member) 	(offsetof(type, member) + sizeof(((type *)0)->member))
#endif

#include "list.h"
#include "main.h"
#include "abi.h"



#define CNA(v, d) VERBS_PCI_MATCH(PCI_VENDOR_ID_##v, d, NULL)
#define BNXT_RE_DEFINE_CNA_TABLE(_name)			\
	static const struct verbs_match_ent	_name[]

#define bnxt_re_wm_barrier()		udma_to_device_barrier()
#define bnxt_re_rm_barrier()		udma_from_device_barrier()

#ifdef HAVE_WR_BIND_MW
#define bnxt_re_is_zero_len_pkt(len, opcd)	((len == 0) &&			\
						 (opcd != IBV_WR_BIND_MW) &&	\
						 (opcd != IBV_WR_LOCAL_INV))
#else
#define bnxt_re_is_zero_len_pkt(len, opcd)	(len == 0)
#endif

#define compat_verbs_init_and_alloc_context(vdev, cmd_fd, cntx, ibvctx) \
	verbs_init_and_alloc_context(vdev, cmd_fd, cntx, ibvctx, RDMA_DRIVER_BNXT_RE)

#define COMPAT_PROVIDER_DRIVER(name, ops) PROVIDER_DRIVER(name, ops)

#ifndef unlikely
#ifdef __GNUC__
#define unlikely(x)	__builtin_expect(!!(x), 0)
#else
#define unlikely(x)	(x)
#endif
#endif

#ifndef likely
#ifdef __GNUC__
#define likely(x)	__builtin_expect(!!(x), 1)
#else
#define likely(x)	(x)
#endif
#endif



static inline int ibv_cmd_create_flow_compat(struct ibv_qp *qp, struct ibv_flow *flow,
					     struct ibv_flow_attr *attr)
{
#ifdef IBV_CMD_CREATE_FLOW_HAS_5_ARG
	return ibv_cmd_create_flow(qp, flow, attr, NULL, 0);
#else
	return ibv_cmd_create_flow(qp, flow, attr);
#endif
}

static inline int ibv_cmd_modify_qp_compat(struct ibv_qp *ibvqp,
					   struct ibv_qp_attr *attr,
					   int attr_mask
#ifdef HAVE_IBV_CMD_MODIFY_QP_EX
					   , bool issue_mqp_ex,
					   struct bnxt_re_modify_qp_ex_req *mreq,
					   struct bnxt_re_modify_qp_ex_resp *mresp
#endif
					  )
{
	int rc;

#ifndef HAVE_IBV_CMD_MODIFY_QP_EX
	struct ibv_modify_qp cmd = {};

	rc = ibv_cmd_modify_qp(ibvqp, attr, attr_mask, &cmd, sizeof(cmd));
#else
	if (issue_mqp_ex) {
		struct bnxt_re_modify_qp_ex_resp *resp;
		struct bnxt_re_modify_qp_ex_req *req;

		req = mreq;
		resp = mresp;
#ifdef IBV_CMD_MODIFY_QP_EX_HAS_9_ARG
		rc = ibv_cmd_modify_qp_ex(ibvqp, attr, attr_mask, &req->cmd,
					  sizeof(req->cmd), sizeof(*req),
					  &resp->resp, sizeof(resp->resp),
					  sizeof(*resp));
#endif
#ifdef IBV_CMD_MODIFY_QP_EX_HAS_7_ARG
		rc = ibv_cmd_modify_qp_ex(ibvqp, attr, attr_mask,
					  &req->cmd, sizeof(*req),
					  &resp->resp, sizeof(*resp));
#endif
	} else {
		struct ibv_modify_qp cmd = {};

		rc = ibv_cmd_modify_qp(ibvqp, attr, attr_mask,
				       &cmd, sizeof(cmd));
	}
#endif
	return rc;
}

static inline int ibv_cmd_create_qp_ex_compat(struct ibv_context *context,
					      struct verbs_qp *qp, int vqp_sz,
					      struct ibv_qp_init_attr_ex *attr_ex,
					      struct ibv_create_qp *cmd, size_t cmd_size,
					      struct ib_uverbs_create_qp_resp *resp,
					      size_t resp_size)
{
#ifdef IBV_CMD_CREATE_QP_EX_HAS_7_ARG
	return ibv_cmd_create_qp_ex(context, qp, attr_ex, cmd,
				    cmd_size, resp, resp_size);
#else
	return ibv_cmd_create_qp_ex(context, qp, vqp_sz, attr_ex, cmd,
				    cmd_size, resp, resp_size);
#endif
}

static inline int ibv_cmd_reg_dmabuf_mr_compat(struct ibv_pd *pd, uint64_t offset, size_t length,
					       uint64_t iova, int fd, int access,
					       struct verbs_mr *vmr,
					       struct ibv_command_buffer *driver)
{
	/* v47 ibv_cmd_reg_dmabuf_mr has 7 args (no driver/ioctl buffer) */
	return ibv_cmd_reg_dmabuf_mr(pd, offset, length, iova, fd, access, vmr);
}


#ifdef SCHED_YIELD_DEFINED
#define pthread_yield()	sched_yield()
#endif

#if !defined(BNXT_RE_ENABLE_DEV_DEBUG)
#define bnxt_re_set_hdr_flags(hdr, sq, wr, slots, sqsig, qt, v3, ilsize) \
			bnxt_re_set_hdr_flags(hdr, wr, slots, sqsig, qt, v3, ilsize)
#endif

#if !defined(ARRAY_SIZE)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef VERBS_CQ_DEFINED
struct verbs_cq {
	union {
		struct ibv_cq cq;
		struct ibv_cq_ex cq_ex;
	};
};
#endif

static inline int ibv_cmd_create_cq_ex_compat(struct ibv_context *context,
#ifdef IBV_CMD_CREATE_CQ_EX_HAS_CONST_CQ_INIT_ATTR
					      const struct ibv_cq_init_attr_ex *cq_attr,
#else
					      struct ibv_cq_init_attr_ex *cq_attr,
#endif
					      struct verbs_cq *cq,
					      struct ibv_create_cq_ex *cmd,
					      size_t cmd_size,
					      struct ib_uverbs_ex_create_cq_resp *resp,
					      size_t resp_size,
					      uint32_t cmd_flags)
{
#ifdef IBV_CMD_CREATE_CQ_EX_HAS_9_ARG
	return ibv_cmd_create_cq_ex(context, cq_attr, NULL, cq, cmd, cmd_size,
				    resp, resp_size, cmd_flags);
#else
#ifdef IBV_CMD_CREATE_CQ_EX_HAS_8_ARG
	return ibv_cmd_create_cq_ex(context, cq_attr, cq, cmd, cmd_size,
				    resp, resp_size, cmd_flags);
#else
	return ibv_cmd_create_cq_ex(context, cq_attr, cq, cmd, cmd_size,
				    resp, resp_size);
#endif
#endif
}
#endif
