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
 * Description: Main component of the bnxt_re user space driver
 */

#ifndef __BNXT_RE_MAIN_H__
#define __BNXT_RE_MAIN_H__

#include "bnxt_re_config.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <endian.h>
#include <pthread.h>
#include <sys/param.h>
#include <stdio.h>
#include <stdarg.h>

/* We always build in-tree against rdma-core. Use ccan/ilog.h's
 * ilog32_nz() helper instead of <math.h> log2() so we don't have to
 * link against libm.
 */
#include <infiniband/driver.h>
#include <util/udma_barrier.h>
#include <util/mmio.h>
#include <ccan/minmax.h>
#include <ccan/ilog.h>

#include "version.h"
#include "compat.h"
#include "abi.h"
#include "memory.h"
#include "list.h"

#define DEV	"bnxt_re : "
#define BNXT_RE_UD_QP_STALL	0x400000
#define PCI_VENDOR_ID_BROADCOM  0x14E4

#define CHIP_NUM_57508		0x1750
#define CHIP_NUM_57504		0x1751
#define CHIP_NUM_57502		0x1752
#define CHIP_NUM_58818          0xd818
#define CHIP_NUM_57608		0x1760
#define CHIP_NUM_57700		0x1770
#define CHIP_NUM_57708		0x1778

#define BNXT_NSEC_PER_SEC	1000000000UL

extern int bnxt_freeze_on_error_cqe;

struct bnxt_re_chip_ctx {
	__u16	chip_num;
	__u8	chip_rev;
	__u8	chip_metal;
	bool	chip_is_gen_p5_p7;
	bool	chip_is_gen_p7;
	bool	chip_is_p8;
	bool	chip_is_p5_plus;
	bool	is_hsi_v3;
};

#define BNXT_RE_PAGE_MASK(pg_size) (~((__u64)(pg_size) - 1))

enum bnxt_re_mmap_flag {
	BNXT_RE_MMAP_SH_PAGE,
	BNXT_RE_MMAP_WC_DB,
	BNXT_RE_MMAP_DBR_PAGE,
	BNXT_RE_MMAP_DB_RECOVERY_PAGE,
	BNXT_RE_MMAP_DBR_PACING_BAR,
	BNXT_RE_MMAP_UC_DB,
	/* Above is fixed index for ABI v7 */
	BNXT_RE_MMAP_TOGGLE_PAGE,
	BNXT_RE_MMAP_HDBR_BASE,
};

#define BNXT_RE_PUSH_MODE_NONE	0
#define BNXT_RE_PUSH_MODE_WCB	1
#define BNXT_RE_PUSH_MODE_PPP	2
#define BNXT_RE_PUSH_MAX_PPP_PER_CTX 8

#define BNXT_RE_DB_REPLAY_YIELD_CNT 256
#define BNXT_RE_DB_KEY_INVALID -1
#define BNXT_RE_MAX_DO_PACING 0xFFFF

#define BNXT_RE_GRC_FIFO_REG_OFFSET 0x21a8

#define BNXT_RE_DB_FIFO_ROOM_MASK_P5    0x1FFF8000
#define BNXT_RE_MAX_FIFO_DEPTH_P5       0x2c00

#define BNXT_RE_DB_FIFO_ROOM_MASK_P7    0x3FFF8000
#define BNXT_RE_MAX_FIFO_DEPTH_P7       0x8000

#define BNXT_RE_DB_FIFO_ROOM_SHIFT      15
#define BNXT_RE_DB_THRESHOLD            20

#define BNXT_RE_DB_FIFO_ROOM_MASK(ctx)  \
	(bnxt_re_is_chip_gen_p7((ctx)) ? \
	 BNXT_RE_DB_FIFO_ROOM_MASK_P7 :\
	 BNXT_RE_DB_FIFO_ROOM_MASK_P5)
#define BNXT_RE_MAX_FIFO_DEPTH(ctx)     \
	(bnxt_re_is_chip_gen_p7((ctx)) ? \
	 BNXT_RE_MAX_FIFO_DEPTH_P7 :\
	 BNXT_RE_MAX_FIFO_DEPTH_P5)

struct bnxt_re_dpi {
	__u32 dpindx;
	__u32 wcdpi;
	__u64 *dbpage;
	__u64 *wcdbpg;
	__u64 dbpage_offset;
#ifdef BNXT_RE_HAVE_DB_LOCK
	pthread_spinlock_t db_lock;
#endif
};

#define BNXT_RE_DB_CTRL_DEFER			(6)
#define BNXT_RE_DB_CTRL_TIMEOUT_NS		(700)
#define BNXT_RE_DB_CTRL_PUSH_TIMEOUT_NS	500
struct bnxt_re_db_ctrl {
	/* timemark for first deferred DB */
	uint64_t	last_tm;
#ifdef BNXT_RE_ENABLE_DEFFER_DB
	/* timemark for first deferred DB */
	uint64_t	mark_tm;
	/* number of outstanding DB requests */
	unsigned int defered;

	/* data to pass to ring callback*/
	void		*opaque;
	/* db ring function callback.  (ring_db)(opaque) */
	void		(*ring_db)(void *db);
	/* Used for priority queue */
	struct bnxt_re_db_ctrl		*_prev;
	/* Used for priority queue */
	struct bnxt_re_db_ctrl		*_next;
	/* when set to true, DB will be rung even if other conditions are not
	 * met.  maybe depreciate
	 */
	bool		force;
#endif
};

struct bnxt_re_pd {
	struct ibv_pd ibvpd;
	uint32_t pdid;
	uint64_t comp_mask;
	uint32_t inline_opt_mw_rkey;
	struct bnxt_re_mem *inline_opt_pad_wqes;
	struct bnxt_re_pd *orig_pd;
	atomic_int refcount;
};

struct bnxt_re_parent_domain {
	struct bnxt_re_pd pd;
	atomic_int refcount;
	struct bnxt_re_td *td;
	void *pd_context;
	void *(*alloc)(struct ibv_pd *pd, void *pd_context, size_t size,
		       size_t alignment, uint64_t flags);
	void (*free)(struct ibv_pd *pd, void *pd_context, void *ptr,
		     uint64_t flags);
};

struct bnxt_re_td {
	struct ibv_td ibvtd;
	atomic_int refcount;
};

struct xorshift32_state {
	/* The state word must be initialized to non-zero */
	uint32_t seed;
};

enum bnxt_dv_cq_flags {
	BNXT_DV_CQ_FLAGS_NONE = 0,
	BNXT_DV_CQ_FLAGS_VALID = 0x1,
	BNXT_DV_CQ_FLAGS_UMEM_REG_DEFAULT = 0x2,
	BNXT_DV_CQ_FLAGS_HELPER = 0x4,
	BNXT_DV_CQ_FLAGS_L2 = 0x8,
};

struct bnxt_re_wc {
	uint64_t wr_id;
	uint64_t com_ns;
	uint32_t src_qp;
};

struct bnxt_re_cq {
	struct verbs_cq verbs_cq;
	struct bnxt_re_list_head sfhead;
	struct bnxt_re_list_head rfhead;
	struct bnxt_re_list_head prev_cq_head;
	struct bnxt_re_context *cntx;
	struct bnxt_re_queue *cqq;
	struct bnxt_re_dpi *udpi;
	struct bnxt_re_mem *resize_mem;
	struct bnxt_re_mem *mem;
	struct bnxt_re_list_node dbnode;
	uint64_t shadow_db_key;
	uint32_t cqe_sz;
	uint32_t cqid;
	struct xorshift32_state rand;
	int deferred_arm_flags;
	bool first_arm;
	bool deferred_arm;
	bool phase;
	uint8_t dbr_lock;
	__u64 *dbc; /* offset 0 of the DB copy block */
	void *toggle_map;
	uint32_t toggle_size;
	uint8_t resize_tog;
	bool deffered_db_sup;
	uint32_t hw_cqes;
	struct bnxt_re_dv_umem *cq_umem;
	int dv_cq_flags;
	bool umem_reg;
	bool ts_cq;
	struct bnxt_re_wc current_wc;
	struct ibv_wc c_wc;
	struct bnxt_re_parent_domain *parent_domain;
};

struct bnxt_re_push_buffer {
	__u64 *pbuf; /*push wc buffer */
	__u64 *wqe; /* hwqe addresses */
	__u64 *ucdb;
	__u64 pstart_key;
	__u64 pend_key;
	uint32_t st_idx;
	uint32_t qpid;
	uint16_t wcdpi;
	uint16_t nbit;
	uint32_t tail;
};

enum bnxt_re_push_info_mask {
	BNXT_RE_PUSH_SIZE_MASK  = 0x1FUL,
	BNXT_RE_PUSH_SIZE_SHIFT = 0x18UL
};

struct bnxt_re_db_ppp_hdr {
	struct bnxt_re_db_hdr db_hdr;
	__u64 rsv_psz_pidx;
};

struct bnxt_re_push_rec {
	struct bnxt_re_dpi *udpi;
	struct bnxt_re_push_buffer *pbuf;
	int pbmap; /* only 16 bits in use */
};

struct bnxt_re_wrid {
	uint64_t wrid;
	int next_idx;
	uint32_t bytes;
	uint8_t sig;
	uint8_t slots;
	uint8_t wc_opcd;
	uint8_t pad_wqe_count;  /* Number of padding WQEs added by inline optimizer */
};

struct bnxt_re_qpcap {
	uint32_t max_swr;
	uint32_t max_rwr;
	uint32_t max_ssge;
	uint32_t max_rsge;
	uint32_t max_inline;
	uint8_t	sqsig;
	uint8_t is_atomic_cap;
};

struct bnxt_re_srq {
	struct ibv_srq ibvsrq;
	struct ibv_srq_attr cap;
	uint32_t srqid;
	struct bnxt_re_context *uctx;
	struct bnxt_re_queue *srqq;
	struct bnxt_re_wrid *srwrid;
	struct bnxt_re_dpi *udpi;
	struct bnxt_re_mem *mem;
	struct bnxt_re_parent_domain *parent_domain;
	int start_idx;
	int last_idx;
	struct bnxt_re_list_node dbnode;
	uint64_t shadow_db_key;
	struct xorshift32_state rand;
	uint8_t dbr_lock;
	__u64 *dbc; /* offset 0 of the DB copy block */
	void *toggle_map;
	uint32_t toggle_size;
};

struct bnxt_re_joint_queue {
	struct bnxt_re_context *cntx;
	struct bnxt_re_queue *hwque;
	struct bnxt_re_wrid *swque;
	uint32_t start_idx;
	uint32_t last_idx;
	uint32_t swque_sz;
};

#ifdef HAVE_IBV_WR_API
/* WR API post send data */
struct bnxt_re_wr_send_qp {
	struct bnxt_re_bsqe     *cur_hdr;
	struct bnxt_re_send     *cur_sqe;
	uint32_t                cur_wqe_cnt;
	uint32_t                cur_slot_cnt;
	uint32_t                used_slot_cnt;
	uint32_t                cur_swq_idx;
	uint8_t                 cur_opcode;
	bool                    cur_push_wqe;
	unsigned int            cur_push_size;
	int                     error;
};
#endif

#define STATIC_WQE_NUM_SLOTS	8
#define SEND_SGE_MIN_SLOTS	3
#define MSG_LEN_ADJ_TO_BYTES	15
#define SLOTS_RSH_TO_NUM_WQE	4

struct bnxt_re_qp {
	struct verbs_qp vqp;
	struct ibv_qp *ibvqp;
	struct bnxt_re_qpcap cap;
	struct bnxt_re_context *cntx;
	struct bnxt_re_chip_ctx *cctx;
	struct bnxt_re_joint_queue *jsqq;
	struct bnxt_re_joint_queue *jrqq;
	struct bnxt_re_dpi *udpi;
	void *pbuf;
	uint64_t wqe_cnt;
	uint16_t mtu;
	uint16_t qpst;
	uint8_t qptyp;
	uint8_t qpmode;
	uint8_t push_st_en;
	uint8_t ppp_idx;
	uint32_t sq_psn;
	uint32_t sq_msn;
	uint32_t qpid;
	uint16_t max_push_sz;
	uint8_t sq_dbr_lock;
	uint8_t rq_dbr_lock;
	struct xorshift32_state rand;
	struct bnxt_re_list_node snode;
	struct bnxt_re_list_node rnode;
	struct bnxt_re_srq *srq;
	struct bnxt_re_cq *rcq;
	struct bnxt_re_cq *scq;
	bool sq_flushed;
	bool rq_flushed;
	struct bnxt_re_mem *mem;/* at cl 6 */
	struct bnxt_re_list_node dbnode;
	uint64_t sq_shadow_db_key;
	uint64_t rq_shadow_db_key;
	__u64 *dbc_sq;
	__u64 *dbc_rq;
	bool     dbc_dt;
	struct bnxt_re_db_ctrl	sq_db_ctrl;
	struct bnxt_re_db_ctrl	rq_db_ctrl;
#ifdef HAVE_IBV_WR_API
	struct bnxt_re_wr_send_qp wr_sq;
#endif
	uint32_t set_ece;
	uint32_t get_ece;
	uint8_t inline_opt_enabled;

	bool exp_mode;

	/* Below members added for DV support */
	struct bnxt_re_qattr qattr[2];
	struct bnxt_re_pd *re_pd;
	struct bnxt_re_parent_domain *parent_domain;
	uint32_t qp_handle;
	struct bnxt_re_dv_umem *sq_umem;
	struct bnxt_re_dv_umem *rq_umem;
	struct bnxt_re_dpi dv_dpi;
};

struct bnxt_re_mr {
#ifndef VERBS_MR_DEFINED
	struct ibv_mr vmr;
#else
	struct verbs_mr vmr;
#endif
};

struct bnxt_re_ah {
	struct ibv_ah ibvah;
	struct bnxt_re_pd *pd;
	uint32_t avid;
};

struct bnxt_re_dev {
	struct verbs_device vdev;
	struct ibv_device_attr devattr;
	uint32_t pg_size;
	uint32_t cqe_size;
	uint32_t max_cq_depth;
	uint32_t db_grc_reg_offset;
	uint32_t db_fifo_max_depth;
	uint32_t db_fifo_room_mask;
	uint8_t db_fifo_room_shift;
	bool small_rx_wqe;
	int driver_abi_version;
};

struct bnxt_re_context {
#ifdef RCP_USE_IB_UVERBS
	struct verbs_context ibvctx;
#else
	struct ibv_context ibvctx;
#endif
	struct bnxt_re_dev *rdev;
	struct bnxt_re_chip_ctx *cctx;
	uint64_t comp_mask;
	struct bnxt_re_dpi udpi;
	uint32_t dev_id;
	uint32_t max_qp;
	uint32_t max_srq;
	uint32_t modes;
	pthread_mutex_t shlock;
	struct bnxt_re_push_rec *pbrec;
	/* TBD - wc_handle is not used in upstream. Fix */
	void *dbr_pacing_page;
	void *dbr_pacing_bar;
	uint64_t replay_cnt;
	uint8_t db_push_mode;
	uint8_t ppp_cnt;
	uint32_t max_rq_wqes;
	uint8_t cq_ignore_overrun;
	uint8_t ece_supported;
	uint8_t deferred_db_enabled;
	uint8_t inline_optimizer_supported;
	uint8_t exp_supported;
	FILE	*dbg_fp;
};

struct bnxt_re_pacing_data {
	uint32_t do_pacing;
	uint32_t pacing_th;
	uint32_t alarm_th;
	uint32_t fifo_max_depth;
	uint32_t fifo_room_mask;
	uint8_t fifo_room_shift;
	uint32_t grc_reg_offset;
	uint32_t dev_err_state;
};

struct bnxt_re_mmap_info {
	__u32 type;
	__u32 dpi;
	__u64 alloc_offset;
	__u32 alloc_size;
	__u32 pg_offset;
	__u32 res_id;
};

/* Chip context related functions */
bool bnxt_re_is_chip_gen_p5(struct bnxt_re_chip_ctx *cctx);
bool bnxt_re_is_chip_gen_p7(struct bnxt_re_chip_ctx *cctx);
bool bnxt_re_is_chip_gen_p5_p7(struct bnxt_re_chip_ctx *cctx);
bool bnxt_re_is_chip_p8(struct bnxt_re_chip_ctx *cctx);

/* DB ring functions used internally*/
void bnxt_re_ring_rq_db(struct bnxt_re_qp *qp);
void bnxt_re_ring_sq_db(struct bnxt_re_qp *qp);
void bnxt_re_ring_exp_db(struct bnxt_re_qp *qp, struct bnxt_re_exp_db *db,
			 bool sq);

void bnxt_re_ring_srq_arm(struct bnxt_re_srq *srq);
void bnxt_re_ring_srq_db(struct bnxt_re_srq *srq);
void bnxt_re_ring_cq_db(struct bnxt_re_cq *cq);
void bnxt_re_ring_cq_arm_db(struct bnxt_re_cq *cq, uint8_t aflag);
void bnxt_re_ring_cq_coff_ack_db(struct bnxt_re_cq *cq, uint8_t aflag);

void bnxt_re_ring_pstart_db(struct bnxt_re_qp *qp,
			    struct bnxt_re_push_buffer *pbuf);
void bnxt_re_ring_pend_db(struct bnxt_re_qp *qp,
			  struct bnxt_re_push_buffer *pbuf);
void bnxt_re_fill_push_wcb(struct bnxt_re_qp *qp,
			   struct bnxt_re_push_buffer *pbuf,
			   uint32_t idx, unsigned int ilsize);

void bnxt_re_fill_ppp(struct bnxt_re_push_buffer *pbuf,
		      struct bnxt_re_qp *qp, uint8_t len, uint32_t idx);
int bnxt_re_init_pbuf_list(struct bnxt_re_context *cntx);
void bnxt_re_destroy_pbuf_list(struct bnxt_re_context *cntx);
void bnxt_re_get_pbuf(struct bnxt_re_qp *qp);
void bnxt_re_put_pbuf(struct bnxt_re_qp *qp);

int bnxt_re_notify_drv(struct ibv_context *ibvctx);
int bnxt_re_get_toggle_mem(struct ibv_context *ibvctx,
			   struct bnxt_re_mmap_info *minfo,
			   uint32_t *page_handle);

void bnxt_re_db_recovery(struct bnxt_re_context *cntx);
int bnxt_re_poll_kernel_cq(struct bnxt_re_cq *cq);
struct ibv_pd *bnxt_re_alloc_parent_domain(struct ibv_context *ibvctx,
					   struct ibv_parent_domain_init_attr *attr);
struct ibv_td *bnxt_re_alloc_td(struct ibv_context *ibvctx,
				struct ibv_td_init_attr *init_attr);
int bnxt_re_dealloc_td(struct ibv_td *ibvtd);

extern int bnxt_single_threaded;
extern uint32_t bnxt_debug_mask;
enum {
	BNXT_DUMP_CONFIG		= 1 << 0,
	BNXT_DUMP_DV			= 1 << 1,
};

#define LEN_50		50
#define bnxt_trace_config(cntx, fmt, ...)		\
{							\
	if (bnxt_debug_mask & BNXT_DUMP_CONFIG)		\
		bnxt_err(cntx, fmt, ##__VA_ARGS__);	\
}

#define bnxt_trace_dv(cntx, fmt, ...)		\
{							\
	if (bnxt_debug_mask & BNXT_DUMP_DV)		\
		bnxt_err(cntx, fmt, ##__VA_ARGS__);	\
}

#define __printf(a, b)   __attribute__((format(printf, a, b)))
static inline void bnxt_err(struct bnxt_re_context *cntx, const char *fmt, ...)
{
	FILE *fp = cntx ? cntx->dbg_fp : stderr;
	char prefix[LEN_50] = {};
	char timestamp[LEN_50];
	struct tm *timeinfo;
	time_t rawtime;
	va_list args;

	time(&rawtime);
	timeinfo = localtime(&rawtime);

	strftime(timestamp, LEN_50, "%b %d %X", timeinfo);
	sprintf(prefix, " %s-%s: ", "rocelib", LIBBNXT_RE_REL_VERSION);

	if (!fp)
		return;
	va_start(args, fmt);
	vfprintf(fp, (char *)&timestamp, args);
	vfprintf(fp, (char *)&prefix, args);
	vfprintf(fp, fmt, args);
	va_end(args);
}

int bnxt_re_alloc_page(struct ibv_context *ibvctx,
		       struct bnxt_re_mmap_info *minfo,
		       uint32_t *page_handle);

/* pointer conversion functions*/
static inline struct bnxt_re_dev *to_bnxt_re_dev(struct ibv_device *ibvdev)
{
#ifdef RCP_USE_IB_UVERBS
	return container_of(ibvdev, struct bnxt_re_dev, vdev.device);
#else
	return container_of(ibvdev, struct bnxt_re_dev, vdev);
#endif
}

static inline struct bnxt_re_context *to_bnxt_re_context(
		struct ibv_context *ibvctx)
{
#ifdef RCP_USE_IB_UVERBS
	return container_of(ibvctx, struct bnxt_re_context, ibvctx.context);
#else
	return container_of(ibvctx, struct bnxt_re_context, ibvctx);
#endif
}

static inline struct bnxt_re_td *to_bnxt_re_td(struct ibv_td *ibvtd)
{
	return container_of(ibvtd, struct bnxt_re_td, ibvtd);
}

static inline struct bnxt_re_pd *to_bnxt_re_pd(struct ibv_pd *ibvpd)
{
	return container_of(ibvpd, struct bnxt_re_pd, ibvpd);
}

static inline struct bnxt_re_parent_domain *bnxt_re_pd_get_parent_domain(struct bnxt_re_pd *pd)
{
	if (!pd || !pd->orig_pd)
		return NULL;
	return container_of(pd, struct bnxt_re_parent_domain, pd);
}

static inline struct bnxt_re_cq *to_bnxt_re_cq(struct ibv_cq *ibvcq)
{
	return container_of(ibvcq, struct bnxt_re_cq, verbs_cq.cq);
}

static inline struct bnxt_re_qp *to_bnxt_re_qp(struct ibv_qp *ibvqp)
{
	struct verbs_qp *vqp = (struct verbs_qp *)ibvqp;

	return container_of(vqp, struct bnxt_re_qp, vqp);
}

static inline struct bnxt_re_srq *to_bnxt_re_srq(struct ibv_srq *ibvsrq)
{
	return container_of(ibvsrq, struct bnxt_re_srq, ibvsrq);
}

static inline struct bnxt_re_ah *to_bnxt_re_ah(struct ibv_ah *ibvah)
{
	return container_of(ibvah, struct bnxt_re_ah, ibvah);
}

/* CQE manipulations */
#define bnxt_re_get_cqe_sz()	(sizeof(struct bnxt_re_req_cqe) +	\
				 sizeof(struct bnxt_re_bcqe))

static inline uint32_t bnxt_re_get_sqe_hdr_sz(struct bnxt_re_chip_ctx *cctx,
					      bool compact)
{
	/* V3 compact size has smaller WQE header */
	if (cctx->is_hsi_v3 && compact)
		return sizeof(struct bnxt_re_bsqe_v3);

	/* everything else (V1/V2/V3 all have the same size) */
	return (sizeof(struct bnxt_re_bsqe) + sizeof(struct bnxt_re_send));
}

static inline uint32_t bnxt_re_get_srqe_hdr_sz(struct bnxt_re_chip_ctx *cctx)
{
	/* V3 has a smaller WQE header */
	if (cctx->is_hsi_v3)
		return sizeof(struct bnxt_re_brqe);

	return (sizeof(struct bnxt_re_brqe) + sizeof(struct bnxt_re_srqe));
}

static inline uint32_t bnxt_re_get_srqe_sz(struct bnxt_re_chip_ctx *cctx, uint32_t nsge)
{
	return bnxt_re_get_srqe_hdr_sz(cctx) + (nsge * sizeof(struct bnxt_re_sge));
}

#define bnxt_re_is_cqe_valid(valid, phase)				\
				(((valid) & BNXT_RE_BCQE_PH_MASK) == (phase))

static inline void bnxt_re_change_cq_phase(struct bnxt_re_cq *cq)
{
	if (!cq->cqq->head)
		cq->phase = !(cq->phase & BNXT_RE_BCQE_PH_MASK);
}

static inline void *bnxt_re_get_swqe(struct bnxt_re_joint_queue *jqq,
				     uint32_t *wqe_idx)
{
	if (wqe_idx)
		*wqe_idx = jqq->start_idx;
	return &jqq->swque[jqq->start_idx];
}

static inline void bnxt_re_jqq_mod_start(struct bnxt_re_joint_queue *jqq,
					 uint32_t idx, uint32_t pad_wqe_count)
{
	jqq->start_idx = jqq->swque[((idx + pad_wqe_count) % jqq->swque_sz)].next_idx;
}

static inline void bnxt_re_jqq_mod_last(struct bnxt_re_joint_queue *jqq,
					uint32_t idx)
{
	jqq->last_idx = jqq->swque[idx].next_idx;
}

static inline void bnxt_re_jqq_mod_last_batch(struct bnxt_re_joint_queue *jqq,
					      uint32_t idx, uint32_t pad_wqe_count)
{
	jqq->last_idx = jqq->swque[((idx + pad_wqe_count) % jqq->swque_sz)].next_idx;
}

static inline uint32_t bnxt_re_init_depth(uint32_t ent, uint64_t cmask)
{
	return cmask & BNXT_RE_UCNTX_CMASK_POW2_DISABLED ?
		ent : roundup_pow_of_two(ent);
}

static inline uint32_t bnxt_re_get_diff(uint64_t cmask)
{
	return cmask & BNXT_RE_UCNTX_CMASK_RSVD_WQE_DISABLED ?
		0 : BNXT_RE_FULL_FLAG_DELTA;
}

static inline int bnxt_re_calc_sq_wqe_sz(struct bnxt_re_chip_ctx *cctx,
					 int nsge, unsigned int ilsize,
					 bool compact)
{
	uint32_t ilslots = 0, slots = 0;

	if (ilsize) {
		ilslots = ilsize / sizeof(struct bnxt_re_sge);
		if (ilsize % sizeof(struct bnxt_re_sge))
			ilslots++;
	}

	slots = max_t(uint32_t, nsge, ilslots);

	return sizeof(struct bnxt_re_sge) * slots + bnxt_re_get_sqe_hdr_sz(cctx, compact);
}

static inline int bnxt_re_calc_rq_wqe_sz(struct bnxt_re_chip_ctx *cctx, int nsge)
{
	if (cctx->is_hsi_v3)
		return sizeof(struct bnxt_re_sge) * nsge + sizeof(struct bnxt_re_brqe);

	return sizeof(struct bnxt_re_sge) * nsge + bnxt_re_get_sqe_hdr_sz(cctx, false);
}

/* Helper function to copy to push buffers */
/* TBD - ilsize is required and it will be posted to upstream */
static inline void bnxt_re_copy_data_to_pb(struct bnxt_re_push_buffer *pbuf,
					   uint8_t offset, uint32_t idx,
					   unsigned int ilsize)
{
	__u64 *src;
	__u64 *dst;
	int indx, i;

	if (unlikely(!ilsize)) {
		/* wrap around of wqe---just walk each wqe*/
		for (indx = 0; indx < idx; indx++) {
			dst = (__u64 *)(pbuf->pbuf + 2 * (indx + offset));
			src = (__u64 *)pbuf->wqe[indx];
			mmio_write64(dst, *src);

			dst++;
			src++;
			mmio_write64(dst, *src);
		}
		return;
	}
	for (indx = 0; indx < 2; indx++) {
		dst = (__u64 *)(pbuf->pbuf + 2 * (indx + offset));
		src = (__u64 *)pbuf->wqe[indx];
		mmio_write64(dst, *src);

		dst++;
		src++;
		mmio_write64(dst, *src);
	}
	dst = (__u64 *)(pbuf->pbuf + 2 * (2 + offset));
	src = (__u64 *)pbuf->wqe[2];
	for (i = 0; i < ilsize; i += 8) {
		mmio_write64(dst, *src);
		dst++;
		src++;
	}
}

static inline int bnxt_re_dp_spin_init(struct bnxt_spinlock *lock, int pshared, int need_lock)
{
	lock->in_use = 0;
	lock->need_lock = need_lock;
	return pthread_spin_init(&lock->lock, PTHREAD_PROCESS_PRIVATE);
}

static inline int bnxt_re_dp_spin_destroy(struct bnxt_spinlock *lock)
{
	return pthread_spin_destroy(&lock->lock);
}

static inline int bnxt_spin_lock(struct bnxt_spinlock *lock)
{
	if (lock->need_lock)
		return pthread_spin_lock(&lock->lock);

	if (unlikely(lock->in_use)) {
		fprintf(stderr, "*** ERROR: multithreading violation ***\n"
			"You are running a multithreaded application but\n"
			"you set BNXT_SINGLE_THREADED=1. Please unset it.\n");
		abort();
	} else {
		lock->in_use = 1;
		 /* This fence is not at all correct, but it increases the */
		 /* chance that in_use is detected by another thread without */
		 /* much runtime cost. */
		atomic_thread_fence(memory_order_acq_rel);
	}

	return 0;
}

static inline int bnxt_spin_unlock(struct bnxt_spinlock *lock)
{
	if (lock->need_lock)
		return pthread_spin_unlock(&lock->lock);

	lock->in_use = 0;
	return 0;
}

static void timespec_sub(const struct timespec *a, const struct timespec *b,
			 struct timespec *res)
{
	res->tv_sec = a->tv_sec - b->tv_sec;
	res->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (res->tv_nsec < 0) {
		res->tv_sec--;
		res->tv_nsec += BNXT_NSEC_PER_SEC;
	}
}

/*
 * Function waits in a busy loop for a given nano seconds
 * The maximum wait period allowed is less than one second
 */
static inline void bnxt_re_sub_sec_busy_wait(uint32_t nsec)
{
	struct timespec start, cur, res;

	if (nsec >= BNXT_NSEC_PER_SEC)
		return;

	if (clock_gettime(CLOCK_REALTIME, &start)) {
		fprintf(stderr, "%s: failed to get time : %d",
			__func__, errno);
		return;
	}

	while (1) {
		if (clock_gettime(CLOCK_REALTIME, &cur)) {
			fprintf(stderr, "%s: failed to get time : %d",
				__func__, errno);
			return;
		}

		timespec_sub(&cur, &start, &res);
		if (res.tv_nsec >= nsec)
			break;
	}
}

#define BNXT_RE_MSN_TBL_EN(a) ((a)->comp_mask & BNXT_RE_UCNTX_CMASK_MSN_TABLE_ENABLED)
#define BNXT_RE_IQM(a) ((a)->comp_mask & BNXT_RE_UCNTX_CMASK_INTERNAL_QUEUE_MEMORY)
#define BNXT_RE_UDP_SP_WQE(a) \
	((a)->comp_mask & BNXT_RE_UCNTX_CMASK_CHG_UDP_SRC_PORT_WQE_SUPPORTED)
#define bnxt_re_dp_spin_lock(lock)     bnxt_spin_lock(lock)
#define bnxt_re_dp_spin_unlock(lock)   bnxt_spin_unlock(lock)
#if defined(BNXT_RE_ENABLE_DEV_DEBUG)
void bnxt_rocelib_test_init(void);
unsigned int bnxt_re_put_tx_sge_test(struct bnxt_re_queue *que, uint32_t *idx,
				     struct ibv_sge *sgl, int nsg);
void bnxt_re_put_rx_sge_test(struct bnxt_re_qp *qp, struct bnxt_re_exp_db *db,
			     struct bnxt_re_queue *que, uint32_t *idx,
			     struct ibv_sge *sgl, int nsg);
uint32_t bnxt_re_rq_hdr_val_test(struct bnxt_re_queue *rq, uint32_t opcd);
uint32_t bnxt_re_sq_hdr_val_test(struct bnxt_re_queue *sq, uint8_t opcd);
void bnxt_dbg_dump_wr(struct ibv_send_wr *wr, struct bnxt_re_qp *qp, void *hdr, void *sqe);
void dump_dbg_msn_tbl(struct bnxt_re_qp *qp, struct bnxt_re_msns *msns,
		      uint32_t len, uint32_t st_idx);
#endif
#endif
