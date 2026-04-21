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
 * Description: Doorbell handling functions.
 */

#include <malloc.h>
#include <unistd.h>

#include "main.h"
#include "compat.h"
#include "bnxt_re_hsi.h"

static uint32_t xorshift32(struct xorshift32_state *state)
{
	/* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
	uint32_t x = state->seed;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return state->seed = x;
}

static uint16_t rnd(struct xorshift32_state *state, uint16_t range)
{
	/* range must be a power of 2 - 1 */
	return (xorshift32(state) & range);
}

static int calculate_fifo_occupancy(struct bnxt_re_context *cntx)
{
	struct bnxt_re_dev *rdev = cntx->rdev;
	uint32_t read_val, fifo_occup;
	uint64_t fifo_reg_off;
	uint32_t *dbr_map;

	fifo_reg_off = rdev->db_grc_reg_offset & ~(BNXT_RE_PAGE_MASK(rdev->pg_size));
	dbr_map = cntx->dbr_pacing_bar + fifo_reg_off;

	read_val = *dbr_map;
	fifo_occup = rdev->db_fifo_max_depth -
		((read_val & rdev->db_fifo_room_mask) >>
		 rdev->db_fifo_room_shift);
	return fifo_occup;
}

static inline uint32_t find_min(uint32_t x, uint32_t y)
{
	return (y > x ? x : y);
}

int bnxt_re_do_pacing(struct bnxt_re_context *cntx, struct xorshift32_state *state)
{
	/* First 4 bytes  of shared page (pacing_info) contains the DBR
	 * pacing information. Second 4 bytes (pacing_th)  contains
	 * the pacing threshold value to determine whether to
	 * add delay or not
	 */
	struct bnxt_re_pacing_data *pacing_data =
		(struct bnxt_re_pacing_data *)cntx->dbr_pacing_page;
	uint32_t wait_time = 1;
	uint32_t fifo_occup;

	if (!pacing_data)
		return 0;
	/* If the device in error recovery state, return error to
	 * not to ring new doorbells in this state.
	 */
	if (pacing_data->dev_err_state)
		return -EFAULT;

	if (rnd(state, BNXT_RE_MAX_DO_PACING) < pacing_data->do_pacing) {
		while ((fifo_occup = calculate_fifo_occupancy(cntx))
				>  pacing_data->pacing_th) {
			uint32_t usec_wait;

			if (pacing_data->alarm_th && fifo_occup > pacing_data->alarm_th)
				bnxt_re_notify_drv(&cntx->ibvctx.context);

			usec_wait = rnd(state, wait_time - 1);
			if (usec_wait)
				bnxt_re_sub_sec_busy_wait(usec_wait * 1000);
			/* wait time capped at 128 us */
			wait_time = find_min(wait_time * 2, 128);
		}
	}
	return 0;
}

static inline void bnxt_re_ring_db(struct bnxt_re_dpi *dpi, __u64 key,
				   uint64_t *db_key, uint8_t *lock)
{
#ifdef BNXT_RE_HAVE_DB_LOCK
	pthread_spin_lock(&dpi->db_lock);
#endif
	while (1) {
		if (__sync_bool_compare_and_swap(lock, 0, 1)) {
			*db_key = key;
			bnxt_re_wm_barrier();
			mmio_write64(dpi->dbpage, key);
			bnxt_re_wm_barrier();
			*lock = 0;
			break;
		}
	}
#ifdef BNXT_RE_HAVE_DB_LOCK
	pthread_spin_unlock(&dpi->db_lock);
#endif
}

static inline void bnxt_re_init_push_hdr(struct bnxt_re_db_hdr *hdr,
					 uint32_t indx, uint32_t qid,
					 uint32_t typ, uint32_t pidx)
{
	__u64 key_lo, key_hi;

	key_lo = (((pidx & BNXT_RE_DB_PILO_MASK) << BNXT_RE_DB_PILO_SHIFT) |
		  (indx & BNXT_RE_DB_INDX_MASK));
	key_hi = ((((pidx & BNXT_RE_DB_PIHI_MASK) << BNXT_RE_DB_PIHI_SHIFT) |
		   (qid & BNXT_RE_DB_QID_MASK)) |
		  ((typ & BNXT_RE_DB_TYP_MASK) << BNXT_RE_DB_TYP_SHIFT) |
		  (0x1UL << BNXT_RE_DB_VALID_SHIFT));
	hdr->typ_qid_indx = htole64((key_lo | (key_hi << 32)));
}

static inline void bnxt_re_init_db_hdr(struct bnxt_re_db_hdr *hdr,
				       uint32_t indx, uint32_t toggle,
				       uint32_t qid, uint32_t typ)
{
	__u64 key_lo, key_hi;

	key_lo = htole32(indx | toggle);
	key_hi = ((qid & BNXT_RE_DB_QID_MASK) |
		  ((typ & BNXT_RE_DB_TYP_MASK) << BNXT_RE_DB_TYP_SHIFT) |
		  (0x1UL << BNXT_RE_DB_VALID_SHIFT));
	hdr->typ_qid_indx = htole64((key_lo | (key_hi << 32)));
}

static inline void __bnxt_re_ring_pend_db(__u64 *ucdb, __u64 key,
					  struct  bnxt_re_qp *qp)
{
	struct bnxt_re_db_hdr hdr;

	bnxt_re_init_db_hdr(&hdr,
			    (*qp->jsqq->hwque->dbtail |
			     ((qp->jsqq->hwque->flags &
			       BNXT_RE_FLAG_EPOCH_TAIL_MASK) <<
			    BNXT_RE_DB_EPOCH_TAIL_SHIFT)), 0,
			    qp->qpid,
			    BNXT_RE_QUE_TYPE_SQ);

	while (1) {
		if (__sync_bool_compare_and_swap(&qp->sq_dbr_lock, 0, 1)) {
			qp->sq_shadow_db_key = hdr.typ_qid_indx;
			bnxt_re_wm_barrier();
			mmio_write64(ucdb, key);
			bnxt_re_wm_barrier();
			qp->sq_dbr_lock = 0;
			break;
		}
	}
}

void bnxt_re_ring_rq_db(struct bnxt_re_qp *qp)
{
	struct bnxt_re_db_hdr hdr;
	uint32_t epoch;
	uint32_t tail;

	if (bnxt_re_do_pacing(qp->cntx, &qp->rand))
		return;

	tail = *qp->jrqq->hwque->dbtail;
	epoch = (qp->jrqq->hwque->flags &  BNXT_RE_FLAG_EPOCH_TAIL_MASK) <<
		BNXT_RE_DB_EPOCH_TAIL_SHIFT;
	bnxt_re_init_db_hdr(&hdr, tail | epoch, 0,
			    qp->qpid, BNXT_RE_QUE_TYPE_RQ);
	if (qp->dbc_rq) {
		*qp->dbc_rq = htole64(hdr.typ_qid_indx);
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(qp->udpi, hdr.typ_qid_indx, &qp->rq_shadow_db_key,
			&qp->rq_dbr_lock);
}

#define DBC_DEBUG_TRACE	(0x1ULL << 59)
static inline void bnxt_re_hdbr_cp_sq(struct bnxt_re_qp *qp, __u64 key)
{
	if (qp->dbc_dt)
		*qp->dbc_sq = htole64(key | DBC_DEBUG_TRACE);
	else
		*qp->dbc_sq = htole64(key);
}

void bnxt_re_ring_sq_db(struct bnxt_re_qp *qp)
{
	struct bnxt_re_db_hdr hdr;
	uint32_t epoch;
	uint32_t tail;

	if (bnxt_re_do_pacing(qp->cntx, &qp->rand))
		return;
	tail = *qp->jsqq->hwque->dbtail;
	epoch = (qp->jsqq->hwque->flags & BNXT_RE_FLAG_EPOCH_TAIL_MASK) <<
		BNXT_RE_DB_EPOCH_TAIL_SHIFT;
	bnxt_re_init_db_hdr(&hdr, tail | epoch, 0, qp->qpid, BNXT_RE_QUE_TYPE_SQ);
	if (qp->dbc_sq) {
		bnxt_re_hdbr_cp_sq(qp, hdr.typ_qid_indx);
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(qp->udpi, hdr.typ_qid_indx, &qp->sq_shadow_db_key,
			&qp->sq_dbr_lock);
}

#if defined(__x86_64__)
static inline void movdir64b(void *dst, const void *src)
{
	/* movdir64b [rdx], rax */
	__asm__ volatile (".byte 0x66, 0x0f, 0x38, 0xf8, 0x02"
			  :
			  : "a" (dst), "d" (src)
			  : "memory");
}
#else
static inline void movdir64b(void *dst, const void *src)
{
}
#endif

#ifdef HAVE_ST64B
static inline void st64b(void *dst, const void *src)
{
	/*
	 * st64b Xt, [Xn]
	 * Atomically store 64 bytes from x0–x7 (starting at src) to the
	 * address in dst.
	 */
	__asm__ volatile("mov x8, %0\n\t"
			 "ldp x0, x1, [%1, #0]\n\t"
			 "ldp x2, x3, [%1, #16]\n\t"
			 "ldp x4, x5, [%1, #32]\n\t"
			 "ldp x6, x7, [%1, #48]\n\t"
			 "st64b x0, [x8]\n\t"
			 :
			 : "r"(dst), "r"(src)
			 : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
			 "memory");
}
#else
static inline void st64b(void *dst, const void *src)
{
}
#endif

void bnxt_re_ring_exp_db(struct bnxt_re_qp *qp, struct bnxt_re_exp_db *db,
			 bool is_sq)
{
	struct bnxt_re_dpi *dpi = qp->udpi;
	void *db_addr = dpi->dbpage;
	uint8_t *lock;

	db->type_xid = htole32(qp->qpid);

	if (is_sq) {
		lock = &qp->sq_dbr_lock;
	} else {
		lock = &qp->rq_dbr_lock;
		db->type_xid |= htole32(BNXT_RE_EXP_DB_TYPE_RQ);
	}

#ifdef BNXT_RE_HAVE_DB_LOCK
	pthread_spin_lock(&dpi->db_lock);
#endif

	db_addr += BNXT_RE_DPI_EXP_DB_OFFSET;
	while (1) {
		if (__sync_bool_compare_and_swap(lock, 0, 1)) {
			bnxt_re_wm_barrier();

			/* express DB requires 64B atomic write */
			movdir64b(db_addr, db);
			st64b(db_addr, db);

			bnxt_re_wm_barrier();
			*lock = 0;
			break;
		}
	}

#ifdef BNXT_RE_HAVE_DB_LOCK
	pthread_spin_unlock(&dpi->db_lock);
#endif
}

void bnxt_re_ring_srq_db(struct bnxt_re_srq *srq)
{
	struct bnxt_re_db_hdr hdr;

	if (bnxt_re_do_pacing(srq->uctx, &srq->rand))
		return;
	bnxt_re_init_db_hdr(&hdr,
			    (srq->srqq->tail |
			     ((srq->srqq->flags &
			       BNXT_RE_FLAG_EPOCH_TAIL_MASK) <<
			     BNXT_RE_DB_EPOCH_TAIL_SHIFT)), 0,
			    srq->srqid, BNXT_RE_QUE_TYPE_SRQ);
	if (srq->dbc) {
		*srq->dbc = htole64(hdr.typ_qid_indx);
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(srq->udpi, hdr.typ_qid_indx, &srq->shadow_db_key,
			&srq->dbr_lock);
}

#define HDBR_SRQ_ARM_OFFSET 2
void bnxt_re_ring_srq_arm(struct bnxt_re_srq *srq)
{
	struct bnxt_re_db_hdr hdr;
	uint32_t toggle = 0;
	uint32_t *pgptr;

	pgptr = (uint32_t *)srq->toggle_map;
	if (pgptr)
		toggle = *pgptr;

	if (bnxt_re_do_pacing(srq->uctx, &srq->rand))
		return;
	bnxt_re_init_db_hdr(&hdr, srq->cap.srq_limit,
			    toggle << BNXT_RE_DB_TOGGLE_SHIFT, srq->srqid,
			    BNXT_RE_QUE_TYPE_SRQ_ARM);
	if (srq->dbc) {
		*(srq->dbc + HDBR_SRQ_ARM_OFFSET) = htole64(hdr.typ_qid_indx);
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(srq->udpi, hdr.typ_qid_indx, &srq->shadow_db_key,
			&srq->dbr_lock);
}

/*
 * During CQ resize, it is expected that the epoch needs to be maintained when
 * switching from the old CQ to the new resized CQ.
 *
 * On the first CQ DB excecuted on the new CQ, we need to check if the index we
 * are writing is less than the last index written for the old CQ. If that is
 * the case, we need to flip the epoch so the ASIC does not get confused and
 * think the CQ DB is out of order and therefore drop the DB (note the logic
 * in the ASIC that checks CQ DB ordering is not aware of the CQ resize).
 */
void bnxt_re_cq_resize_check(struct bnxt_re_queue *cqq)
{
	if (unlikely(cqq->cq_resized)) {
		if (cqq->head < cqq->old_head)
			cqq->flags ^= 1UL << BNXT_RE_FLAG_EPOCH_HEAD_SHIFT;

		cqq->cq_resized = false;
	}
}

#define HDBR_CQ_ARM_OFFSET 1
#define HDBR_CQ_CUTOFF_ACK 2
#define HDBR_CQ_OFFSET     3
void bnxt_re_ring_cq_db(struct bnxt_re_cq *cq)
{
	struct bnxt_re_db_hdr hdr;

	if (bnxt_re_do_pacing(cq->cntx, &cq->rand))
		return;

	bnxt_re_cq_resize_check(cq->cqq);

	bnxt_re_init_db_hdr(&hdr,
			    (cq->cqq->head |
			     ((cq->cqq->flags &
			       BNXT_RE_FLAG_EPOCH_HEAD_MASK) <<
			     BNXT_RE_DB_EPOCH_HEAD_SHIFT)), 0,
			    cq->cqid,
			    BNXT_RE_QUE_TYPE_CQ);
	if (cq->dbc) {
		*(cq->dbc + HDBR_CQ_OFFSET) = htole64(hdr.typ_qid_indx);
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(cq->udpi, hdr.typ_qid_indx, &cq->shadow_db_key,
			&cq->dbr_lock);
}

static inline int bnxt_re_cq_arm_dbc_offset(uint8_t aflag)
{
	if (aflag == BNXT_RE_QUE_TYPE_CQ_CUT_ACK)
		return HDBR_CQ_CUTOFF_ACK;
	return HDBR_CQ_ARM_OFFSET; /* ARMSE/ARMALL */
}

void bnxt_re_ring_cq_coff_ack_db(struct bnxt_re_cq *cq, uint8_t aflag)
{
	struct bnxt_re_db_hdr hdr;
	uint32_t toggle = 0;

	toggle = cq->resize_tog;

	if (bnxt_re_do_pacing(cq->cntx, &cq->rand))
		return;
	bnxt_re_init_db_hdr(&hdr,
			    (cq->cqq->head |
			     ((cq->cqq->flags &
			       BNXT_RE_FLAG_EPOCH_HEAD_MASK) <<
			     BNXT_RE_DB_EPOCH_HEAD_SHIFT)),
			     toggle << BNXT_RE_DB_TOGGLE_SHIFT,
			    cq->cqid, aflag);
	if (cq->dbc) {
		*(cq->dbc + bnxt_re_cq_arm_dbc_offset(aflag)) = hdr.typ_qid_indx;
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(cq->udpi, hdr.typ_qid_indx, &cq->shadow_db_key,
			&cq->dbr_lock);
}

void bnxt_re_ring_cq_arm_db(struct bnxt_re_cq *cq, uint8_t aflag)
{
	struct bnxt_re_db_hdr hdr;
	uint32_t toggle = 0;
	uint32_t *pgptr;

	pgptr = (uint32_t *)cq->toggle_map;
	if (pgptr)
		toggle = *pgptr;

	if (bnxt_re_do_pacing(cq->cntx, &cq->rand))
		return;

	bnxt_re_cq_resize_check(cq->cqq);

	bnxt_re_init_db_hdr(&hdr,
			    (cq->cqq->head |
			     ((cq->cqq->flags &
			       BNXT_RE_FLAG_EPOCH_HEAD_MASK) <<
			     BNXT_RE_DB_EPOCH_HEAD_SHIFT)),
			     toggle << BNXT_RE_DB_TOGGLE_SHIFT,
			    cq->cqid, aflag);
	if (cq->dbc) {
		*(cq->dbc + bnxt_re_cq_arm_dbc_offset(aflag)) = htole64(hdr.typ_qid_indx);
		bnxt_re_wm_barrier();
	}
	bnxt_re_ring_db(cq->udpi, hdr.typ_qid_indx, &cq->shadow_db_key,
			&cq->dbr_lock);
}

void bnxt_re_ring_pstart_db(struct bnxt_re_qp *qp,
			    struct bnxt_re_push_buffer *pbuf)
{
	__u64 key;

	if (bnxt_re_do_pacing(qp->cntx, &qp->rand))
		return;

	key = (pbuf->pstart_key | (pbuf->st_idx & BNXT_RE_DB_INDX_MASK));
	if (qp->dbc_sq)
		bnxt_re_hdbr_cp_sq(qp, key);
	bnxt_re_wm_barrier();
	mmio_write64(pbuf->ucdb, key);
}

void bnxt_re_ring_pend_db(struct bnxt_re_qp *qp,
			  struct bnxt_re_push_buffer *pbuf)
{
	__u64 key;

	if (bnxt_re_do_pacing(qp->cntx, &qp->rand))
		return;

	key = (pbuf->pend_key | (pbuf->tail & BNXT_RE_DB_INDX_MASK));
	__bnxt_re_ring_pend_db(pbuf->ucdb, key, qp);
}

void bnxt_re_fill_ppp(struct bnxt_re_push_buffer *pbuf,
		      struct bnxt_re_qp *qp, uint8_t len, uint32_t idx)
{
	struct bnxt_re_db_ppp_hdr phdr = {};
	__u64 *dst, *src;
	__u8 plen;
	int indx;

	src = (__u64 *)&phdr;
	plen = len + sizeof(phdr) + bnxt_re_get_sqe_hdr_sz(qp->cctx, false);

	bnxt_re_init_db_hdr(&phdr.db_hdr,
			    (*qp->jsqq->hwque->dbtail |
			     ((qp->jsqq->hwque->flags &
			       BNXT_RE_FLAG_EPOCH_TAIL_MASK) <<
			     BNXT_RE_DB_EPOCH_TAIL_SHIFT)), 0,
			    qp->qpid,
			    BNXT_RE_QUE_TYPE_SQ);

	phdr.rsv_psz_pidx = ((pbuf->st_idx & BNXT_RE_DB_INDX_MASK) |
			     (((plen % 8 ? (plen / 8) + 1 :
				plen / 8) & BNXT_RE_PUSH_SIZE_MASK) <<
			       BNXT_RE_PUSH_SIZE_SHIFT));

	if (qp->dbc_sq)
		bnxt_re_hdbr_cp_sq(qp, *src);
	bnxt_re_wm_barrier();
	for (indx = 0; indx < 2; indx++) {
		dst = (__u64 *)(pbuf->pbuf + indx);
		mmio_write64(dst, *src);
		src++;
	}
	bnxt_re_copy_data_to_pb(pbuf, 1, idx, 0);
	mmio_flush_writes();
}

void bnxt_re_fill_push_wcb(struct bnxt_re_qp *qp,
			   struct bnxt_re_push_buffer *pbuf, uint32_t idx,
			   unsigned int ilsize)
{
	bnxt_re_ring_pstart_db(qp, pbuf);
	mmio_wc_start();
	bnxt_re_copy_data_to_pb(pbuf, 0, idx, ilsize);
	/* Flush WQE write before push end db. */
	mmio_flush_writes();
	bnxt_re_ring_pend_db(qp, pbuf);
}

int bnxt_re_init_pbuf_list(struct bnxt_re_context *ucntx)
{
	struct bnxt_re_push_buffer *pbuf;
	int indx, wqesz;
	int size, offt;
	__u64 wcpage;
	__u64 dbpage;
	void *base;

	size = (sizeof(*ucntx->pbrec) +
		16 * (sizeof(*ucntx->pbrec->pbuf) +
		      sizeof(struct bnxt_re_push_wqe)));
	ucntx->pbrec = calloc(1, size);
	if (!ucntx->pbrec)
		goto out;

	offt = sizeof(*ucntx->pbrec);
	base = ucntx->pbrec;
	ucntx->pbrec->pbuf = (base + offt);
	ucntx->pbrec->pbmap = 0x7fff; /* 15 bits */
	ucntx->pbrec->udpi = &ucntx->udpi;

	wqesz = sizeof(struct bnxt_re_push_wqe);
	wcpage = (__u64)ucntx->udpi.wcdbpg;
	dbpage = (__u64)ucntx->udpi.dbpage;
	offt = sizeof(*ucntx->pbrec->pbuf) * 16;
	base = (char *)ucntx->pbrec->pbuf + offt;
	for (indx = 0; indx < 16; indx++) {
		pbuf = &ucntx->pbrec->pbuf[indx];
		pbuf->wqe = base + indx * wqesz;
		pbuf->pbuf = (__u64 *)(wcpage + indx * wqesz);
		pbuf->ucdb = (__u64 *)(dbpage + (indx + 1) * sizeof(__u64));
		pbuf->wcdpi = ucntx->udpi.wcdpi;
	}

	return 0;
out:
	return -ENOMEM;
}

void bnxt_re_build_dbkey(struct bnxt_re_qp *qp)
{
	struct bnxt_re_push_buffer *pbuf = qp->pbuf;
	__u64 key = 0;

	key = ((((pbuf->wcdpi & BNXT_RE_DB_PIHI_MASK) << BNXT_RE_DB_PIHI_SHIFT) |
			(qp->qpid & BNXT_RE_DB_QID_MASK)) |
			((BNXT_RE_PUSH_TYPE_START & BNXT_RE_DB_TYP_MASK)
					<< BNXT_RE_DB_TYP_SHIFT));
	key <<= 32;
	key |= (((__u32)pbuf->wcdpi & BNXT_RE_DB_PILO_MASK) << BNXT_RE_DB_PILO_SHIFT);
	pbuf->pstart_key = key;

	key = 0;
	key = ((((pbuf->wcdpi & BNXT_RE_DB_PIHI_MASK) << BNXT_RE_DB_PIHI_SHIFT) |
			(qp->qpid & BNXT_RE_DB_QID_MASK)) |
			((BNXT_RE_PUSH_TYPE_END & BNXT_RE_DB_TYP_MASK)
					<< BNXT_RE_DB_TYP_SHIFT));
	key <<= 32;
	key |= (((__u32)pbuf->wcdpi & BNXT_RE_DB_PILO_MASK) << BNXT_RE_DB_PILO_SHIFT);
	pbuf->pend_key = key;
}

void bnxt_re_get_pbuf(struct bnxt_re_qp *qp)
{
	struct bnxt_re_push_buffer *pbuf = NULL;
	struct bnxt_re_context *cntx = qp->cntx;
	uint8_t *push_st_en = &qp->push_st_en;
	uint8_t ppp_idx = qp->ppp_idx;
	uint8_t buf_state = 0;
	int bit;

	switch (cntx->db_push_mode) {
	case BNXT_RE_PUSH_MODE_PPP:
		buf_state = !!(*push_st_en & BNXT_RE_PPP_STATE_MASK);
		pbuf = &cntx->pbrec->pbuf[(ppp_idx * 2) + buf_state];
		/* Flip */
		*push_st_en ^= 1UL << BNXT_RE_PPP_ST_SHIFT;
		break;

	case BNXT_RE_PUSH_MODE_WCB:
		pthread_mutex_lock(&qp->cntx->shlock);
		bit = __builtin_ffs(cntx->pbrec->pbmap);
		if (bit) {
			pbuf = &cntx->pbrec->pbuf[bit];
			pbuf->nbit = bit;
			cntx->pbrec->pbmap &= ~(0x1 << (bit - 1));
		}
		pthread_mutex_unlock(&qp->cntx->shlock);
		break;
	}

	qp->pbuf = pbuf;
	if (pbuf && bnxt_re_is_chip_gen_p5(qp->cctx))
		bnxt_re_build_dbkey(qp);

	return;
}

void bnxt_re_put_pbuf(struct bnxt_re_qp *qp)
{
	struct bnxt_re_push_buffer *pbuf = qp->pbuf;
	struct bnxt_re_context *cntx = qp->cntx;
	int bit;

	if (cntx->cctx->chip_is_gen_p7)
		return;

	pthread_mutex_lock(&qp->cntx->shlock);
	if (pbuf->nbit) {
		bit = pbuf->nbit;
		cntx->pbrec->pbmap |= (0x1 << (bit - 1));
		pbuf->nbit = 0;
		pbuf->pstart_key = 0;
		pbuf->pend_key = 0;
		qp->pbuf = NULL;
	}
	pthread_mutex_unlock(&qp->cntx->shlock);
}

void bnxt_re_destroy_pbuf_list(struct bnxt_re_context *cntx)
{
	free(cntx->pbrec);
}
