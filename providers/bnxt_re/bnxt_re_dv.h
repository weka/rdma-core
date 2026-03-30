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

#ifndef __BNXT_RE_DV_H__
#define __BNXT_RE_DV_H__

#include <stdint.h>
#include <infiniband/verbs.h>
#ifdef __cplusplus
extern "C" {
#endif

struct bnxt_re_dv_qp {
	uint64_t wqe_cnt;
	uint64_t		comp_mask;
};

struct bnxt_re_dv_cq {
	uint32_t		cqn;
	uint32_t		cqe_size;
	uint64_t		comp_mask;
};

struct bnxt_re_dv_srq {
	uint32_t		srqn;
	uint64_t		comp_mask;
};

struct bnxt_re_dv_ah {
	uint32_t		avid;
	uint64_t		comp_mask;
};

struct bnxt_re_dv_pd {
	uint32_t		pdn;
	uint64_t		comp_mask;
};

struct bnxt_re_dv_obj {
	struct {
		struct ibv_qp		*in;
		struct bnxt_re_dv_qp	*out;
	} qp;
	struct {
		struct ibv_cq		*in;
		struct bnxt_re_dv_cq	*out;
	} cq;
	struct {
		struct ibv_srq		*in;
		struct bnxt_re_dv_srq	*out;
	} srq;
	struct {
		struct ibv_ah		*in;
		struct bnxt_re_dv_ah	*out;
	} ah;
	struct {
		struct ibv_pd		*in;
		struct bnxt_re_dv_pd	*out;
	} pd;
};

int bnxt_re_dv_init_obj(struct bnxt_re_dv_obj *obj, uint64_t obj_type);

enum bnxt_re_roce_dv_obj_type {
	BNXT_RE_DV_OBJ_QP       = 1 << 0,
	BNXT_RE_DV_OBJ_CQ       = 1 << 1,
	BNXT_RE_DV_OBJ_SRQ      = 1 << 2,
	BNXT_RE_DV_OBJ_AH       = 1 << 3,
	BNXT_RE_DV_OBJ_PD       = 1 << 4,
};

int bnxt_re_dv_modify_qp_udp_sport(struct ibv_qp *qp, uint16_t udp_sport);

struct bnxt_re_dv_db_region_attr {
	uint32_t handle;
	uint32_t dpi;
	uint64_t umdbr;
	__u64 *dbr;
};

struct bnxt_re_dv_db_region_attr *
bnxt_re_dv_alloc_db_region(struct ibv_context *ctx);
int bnxt_re_dv_free_db_region(struct ibv_context *ctx,
			      struct bnxt_re_dv_db_region_attr *attr);
int bnxt_re_dv_get_default_db_region(struct ibv_context *ibvctx,
				     struct bnxt_re_dv_db_region_attr *out);

enum  bnxt_re_dv_umem_in_flags {
	BNXT_RE_DV_UMEM_FLAGS_DMABUF = 1 << 0,
	BNXT_RE_DV_UMEM_FLAGS_MAP = 1 << 1,
};

struct bnxt_re_dv_umem_reg_attr {
	void *addr;
	size_t size;
	uint32_t access_flags;
	uint64_t pgsz_bitmap;
	uint64_t comp_mask;
	int dmabuf_fd;
	void *pa_buf;
	size_t pa_buf_sz;
	uint32_t map_pg_sz;
};

/* Direct Verbs CQ comp_mask flags */
enum bnxt_re_dv_cq_comp_mask {
	BNXT_RE_DV_CQ_FLAG_L2 = 1ULL << 0, /* L2 CQ */
};

struct bnxt_re_dv_cq_init_attr {
	uint64_t cq_handle;
	void *umem_handle; /* umem_handle from umem_reg */
	uint64_t cq_umem_offset;	/* offset into umem */
	uint32_t ncqe;
	uint64_t comp_mask; /* compatibility mask for future updates */
	uint32_t cqid; /* output */
};

struct bnxt_re_dv_cq_attr {
	uint32_t ncqe; /* no. of entries */
	uint32_t cqe_size; /* size of entries */
};

struct bnxt_re_dv_qp_init_attr {
	/* Standard ibv params */
	enum ibv_qp_type qp_type;
	uint32_t max_send_wr;
	uint32_t max_recv_wr;
	uint32_t max_send_sge;
	uint32_t max_recv_sge;
	uint32_t max_inline_data;
	struct ibv_cq *send_cq;
	struct ibv_cq *recv_cq;
	struct ibv_srq *srq;

	/* DV params */
	uint64_t qp_handle;	/* to match with cqe */
	void *dbr_handle;	/* dbr_handle from alloc_dbr */
	void *sq_umem_handle;	/* umem_handle from umem_reg */
	uint64_t sq_umem_offset;	/* offset into umem */
	uint32_t sq_len;	/* sq length including MSN area */
	uint32_t sq_slots;	/* sq length in slots */
	void *rq_umem_handle;	/* umem_handle from umem_reg */
	uint64_t rq_umem_offset;	/* offset into umem */
	uint32_t sq_wqe_sz;     /* sq wqe size */
	uint32_t sq_psn_sz;     /* sq psn size */
	uint32_t sq_npsn;       /* sq num psn entries */
	uint32_t rq_len;	/* rq length */
	uint32_t rq_slots;	/* rq length in slots */
	uint32_t rq_wqe_sz;     /* rq wqe size */
	uint64_t comp_mask;	/* compatibility mask for future updates */
};

struct bnxt_re_dv_qp_mem_info {
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

struct bnxt_re_dv_stats_ctx_attr {
	uint32_t update_period_ms;
	uint16_t stats_dma_length;
	uint32_t stats_size;       /* Size of stats buffer */
};

struct bnxt_re_dv_ring_attr {
	uint16_t logical_id;    /* Logical ring ID */
	uint32_t depth;		/* Number of descriptors */
	uint16_t rx_buf_size;   /* Applicable for Rx ring only. Buf size */
	uint16_t dpi;		/* Default DB region handle */
	uint8_t  mpc_chnl_type;	/* MPC channel type (TCE, RCE, TE_CFA, RE_CFA) */
	uint8_t traffic_class;  /* Traffic class to be used for Tx */
};

struct bnxt_re_dv_vnic_attr {
	uint32_t flags;
	uint16_t virtio_net_fid;
	uint16_t vnic_id;
};

struct bnxt_re_dv_bfid_attr {
	uint32_t flags;
	uint16_t bfid;
};

enum bnxt_re_dv_obj_type {
	BNXT_RE_OBJ_TYPE_TX,
	BNXT_RE_OBJ_TYPE_RX,
	BNXT_RE_OBJ_TYPE_RX_AGGR,
	BNXT_RE_OBJ_TYPE_RSS,
	BNXT_RE_OBJ_TYPE_STATS,
	BNXT_RE_OBJ_TYPE_VNIC,
	BNXT_RE_OBJ_TYPE_MPC,
	BNXT_RE_OBJ_TYPE_BFID,
};

/* Response structures */
struct bnxt_re_dv_ring_resp {
	uint16_t ring_id;		/* Allocated ring ID */
	uint64_t comp_mask;		/* For future expansion */
};

struct bnxt_re_dv_stats_ctx_resp {
	__u32 stat_ctx_id;		/* Allocated stats context ID */
	__aligned_u64 comp_mask;	/* For future expansion */
};

struct bnxt_re_dv_vnic_resp {
	uint16_t vnic_id;		/* Allocated VNIC ID */
	uint64_t comp_mask;		/* For future expansion */
};

struct bnxt_re_dv_bfid_resp {
	uint16_t bfid;			/* Allocated bfid */
	uint64_t comp_mask;		/* For future expansion */
};

struct bnxt_re_dv_rss_ctx_resp {
	uint16_t vnic_id;		/* VNIC ID used */
	uint16_t rss_ctx_idx;		/* RSS context index */
	uint64_t comp_mask;		/* For future expansion */
};

struct bnxt_re_dv_create_obj_resp {
	__u8 obj_type;			/* Echo back the object type */
	union {
		struct bnxt_re_dv_ring_resp ring_resp;
		struct bnxt_re_dv_stats_ctx_resp stats_resp;
		struct bnxt_re_dv_vnic_resp vnic_resp;
		struct bnxt_re_dv_rss_ctx_resp rss_resp;
		struct bnxt_re_dv_bfid_resp bfid_resp;
	};
};

struct bnxt_re_dv_create_obj_init_attr {
	uint8_t obj_type;       /* BNXT_RE_OBJ_TYPE_TX, RX, RX_AGGR, RSS, STATS, VNIC */
	uint8_t pg_size;        /* Page size */
	void *umem;             /* return value from bnxt_re_dv_umem_reg */
	uint64_t offset;        /* Offset into umem */
	uint32_t size;
	uint32_t handle;
	struct ibv_cq *send_cq;
	uint16_t stats_handle;
	uint16_t rx_handle;     /* For RX_AGGR: handle to parent RX ring object */
	union {
		struct bnxt_re_dv_stats_ctx_attr stats_attr;
		struct bnxt_re_dv_ring_attr ring_attr;
		struct bnxt_re_dv_vnic_attr vnic_attr;
		struct bnxt_re_dv_bfid_attr  bfid_attr;
	};
	uint64_t comp_mask; /* For future expansion */
};

enum bnxt_re_dv_pt_msg_type {
	BNXT_RE_DV_PT_TYPE_TF,
};

struct bnxt_re_dv_pt_msg_input {
	uint8_t pt_type;
	uint64_t req;
	uint16_t req_len;
	uint64_t resp;
	uint16_t resp_len;
};

int bnxt_re_dv_send_pt(struct ibv_context *ibvctx,
		       struct bnxt_re_dv_pt_msg_input *in);
void *bnxt_re_dv_umem_reg(struct ibv_context *ibvctx,
			  struct bnxt_re_dv_umem_reg_attr *in);
int bnxt_re_dv_umem_dereg(void *umem_handle);
struct ibv_cq *bnxt_re_dv_create_cq(struct ibv_context *ibvctx,
				    struct bnxt_re_dv_cq_init_attr *cq_attr);
int bnxt_re_dv_destroy_cq(struct ibv_cq *ibv_cq);
struct ibv_qp *bnxt_re_dv_create_qp(struct ibv_pd *pd,
				    struct bnxt_re_dv_qp_init_attr *qp_attr);
int bnxt_re_dv_destroy_qp(struct ibv_qp *ibvqp);
int bnxt_re_dv_modify_qp(struct ibv_qp *ibv_qp, struct ibv_qp_attr *attr,
			 int attr_mask, uint32_t type, uint32_t value);
int bnxt_re_dv_query_qp(void *qp_handle, struct ib_uverbs_qp_attr *attr);
int bnxt_re_dv_qp_mem_alloc(struct ibv_pd *ibvpd,
			    struct ibv_qp_init_attr *attr,
			    struct bnxt_re_dv_qp_mem_info *dv_qp_mem);
int bnxt_re_dv_qp_get_mem_info(struct ibv_pd *ibvpd,
			       struct ibv_qp_init_attr *attr,
			       struct bnxt_re_dv_qp_mem_info *qp_mem);
int bnxt_re_dv_get_cq_attr(struct ibv_context *ibvctx, uint32_t ncqe,
			   struct bnxt_re_dv_cq_attr *cq_attr);
void *bnxt_re_dv_cq_mem_alloc(struct ibv_context *ibvctx, int num_cqe,
			      struct bnxt_re_dv_cq_attr *cq_attr);
int bnxt_re_dv_send_fw_msg(struct ibv_context *ctx, void *in, int in_size,
			   void *out, int out_size);
int bnxt_re_dv_create_obj(struct ibv_context *ibvctx,
			  struct bnxt_re_dv_create_obj_init_attr *attr,
			  struct bnxt_re_dv_create_obj_resp *resp);
int bnxt_re_dv_destroy_obj(struct ibv_context *ibvctx, uint32_t handle);
#ifdef __cplusplus
}
#endif
#endif /* __BNXT_RE_DV_H__ */
