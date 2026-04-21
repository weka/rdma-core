/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, Broadcom. All rights reserved.  The term
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
 * Description: Direct verb support user interface header
 */

#ifndef __BNXT_RE_DV_INTERNAL_H__
#define __BNXT_RE_DV_INTERNAL_H__

#include <stdint.h>
#include <infiniband/verbs.h>

struct bnxt_re_dv_umem {
	struct ibv_context *context;
	uint32_t handle;
	void *addr;
	size_t size;

	/* The below member is only used if QP memory helper
	 * API is utilized by the application, to track QPs
	 * that share the same umem. Note that the app is
	 * expected to allocate memory for SQs and RQs in
	 * two separate chunks.
	 */
	int umem_qp_count;
};

struct bnxt_re_dv_qp_init_attr_internal {
	/* Standard ibv params */
	enum ibv_qp_type qp_type;
	uint32_t max_send_wr;
	uint32_t max_recv_wr;
	uint32_t max_send_sge;
	uint32_t max_recv_sge;
	uint32_t max_inline_data;
	uint32_t pdid;
	void *send_cq;
	void *recv_cq;

	/* DV params */
	uint64_t qp_handle;     /* to match with cqe */
	uint64_t sq_va;         /* Peer-mem sq-va (not dma mapped) */
	uint32_t sq_len;        /* sq length including MSN area */
	uint32_t sq_slots;      /* sq length in slots */
	uint32_t sq_wqe_sz;     /* sq wqe size */
	uint32_t sq_psn_sz;     /* sq psn size */
	uint32_t sq_npsn;       /* sq num psn entries */
	uint64_t rq_va;         /* Peer-mem rq-va (not dma mapped) */
	uint32_t rq_len;        /* rq length */
	uint32_t rq_slots;      /* rq length in slots */
	uint32_t rq_wqe_sz;     /* rq wqe size */
	uint64_t comp_mask;     /* compatibility bit mask */
};

#endif /* __BNXT_RE_DV_INTERNAL_H__ */
