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
 * Description: rocelib test suite of the bnxt_re driver
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#if defined(__x86_64__)
#include <x86intrin.h>
static inline uint64_t bnxt_dbg_read_cycles(void)
{
	unsigned int dummy;

	asm volatile("lfence" ::: "memory");
	return __rdtscp(&dummy);
}
#elif defined(__aarch64__)
static inline uint64_t bnxt_dbg_read_cycles(void)
{
	uint64_t val;

	asm volatile("isb" ::: "memory");
	asm volatile("mrs %0, cntvct_el0" : "=r" (val));
	return val;
}
#else
static inline uint64_t bnxt_dbg_read_cycles(void) { return 0; }
#endif

#include "main.h"
#include "bnxt_re_hsi.h"
#include "verbs.h"

#ifndef BIT
#define BIT(x)  (1UL << (x))
#endif

__u32 bnxt_err_sq_idx;
__u32 bnxt_err_rq_idx;

enum {
	BNXT_ERR_INJECT_LOCAL_PROT	= 1 << 0,
	BNXT_ERR_INJECT_LOCAL_LEN	= 1 << 1,
	BNXT_ERR_INJECT_OPCODE		= 1 << 2,
	/* Add err inject enum above */
	BNXT_DBG_DUMP_WR		= 1 << 8,
	BNXT_DBG_DUMP_MSN		= 1 << 9,
	BNXT_DBG_DUMP_CYCLES		= 1 << 10,
};

uint32_t bnxt_dev_debug_mask;
static uint32_t bnxt_dbg_cycle_cnt_mod = BNXT_DBG_CYCLE_CNT_DEFAULT_MOD;
struct bnxt_dbg_cycle_cnt exp_send_cycle_cnt;

__u32 err_sq_idx_get(void)
{
	char *env;

	env = getenv("BNXT_ERR_INJECT_SQ_IDX");
	if (env)
		return strtoul(env, NULL, 0);

	return 0;
}

__u32 err_rq_idx_get(void)
{
	char *env;

	env = getenv("BNXT_ERR_INJECT_RQ_IDX");
	if (env)
		return strtoul(env, NULL, 0);

	return 0;
}

int bnxt_re_test(int i)
{
	fprintf(stderr, "This is a test %s val %d\n", __func__, i);
	return 0;
}

void bnxt_rocelib_test_init(void)
{
	char *env;

	env = getenv("BNXT_DEBUG_MASK_INTERNAL");
	if (env)
		bnxt_dev_debug_mask = strtol(env, NULL, 0);

	if (bnxt_dev_debug_mask & BNXT_DBG_DUMP_CYCLES) {
		env = getenv("BNXT_DBG_CYCLE_CNT_MOD");
		if (env) {
			bnxt_dbg_cycle_cnt_mod =
				strtoul(env, NULL, 0);
			if (bnxt_dbg_cycle_cnt_mod == 0)
				bnxt_dbg_cycle_cnt_mod =
					BNXT_DBG_CYCLE_CNT_DEFAULT_MOD;
		}
		fprintf(stderr, DEV "Cycle count moderation set to %u\n",
			bnxt_dbg_cycle_cnt_mod);
		bnxt_dbg_cycle_cnt_init(&exp_send_cycle_cnt, 5, 0,
					"exp_post_send");
	}

	bnxt_err_sq_idx = err_sq_idx_get();
	if (bnxt_err_sq_idx)
		printf(DEV "Error SQ index set to 0x%x\n", bnxt_err_sq_idx);

	bnxt_err_rq_idx = err_rq_idx_get();
	if (bnxt_err_rq_idx)
		printf(DEV "Error RQ index set to 0x%x\n", bnxt_err_rq_idx);
}

unsigned int bnxt_re_put_tx_sge_test(struct bnxt_re_queue *que, uint32_t *idx,
				     struct ibv_sge *sgl, int nsg)
{
	struct bnxt_re_sge *sge;
	int indx;
	int len;

	len = 0;
	for (indx = 0; indx < nsg; indx++) {
		sge = bnxt_re_get_hwqe(que, (*idx)++);
		/*
		 * Corrupt SGE PA at the desired SQ index if error flag is
		 * set
		 */
		if ((que->tail == bnxt_err_sq_idx) &&
		    (bnxt_dev_debug_mask & BNXT_ERR_INJECT_LOCAL_PROT))
			sge->pa = 0;
		else
			sge->pa = htole64(sgl[indx].addr);

		sge->lkey = htole32(sgl[indx].lkey);
		/*
		 * Corrupt SGE length at the desired SQ index if error flag is
		 * set
		 */
		if ((que->tail == bnxt_err_sq_idx) &&
		    (bnxt_dev_debug_mask & BNXT_ERR_INJECT_LOCAL_LEN))
			sge->length = 0xffffffff;
		else
			sge->length = htole32(sgl[indx].length);

		len += sgl[indx].length;
	}
	return len;
}

void bnxt_re_put_rx_sge_test(struct bnxt_re_qp *qp, struct bnxt_re_exp_db *db,
			     struct bnxt_re_queue *que, uint32_t *idx,
			     struct ibv_sge *sgl, int nsg)
{
	struct bnxt_re_sge *sge;
	int indx;

	for (indx = 0; indx < nsg; indx++) {
		sge = qp->exp_mode ? &db->wqe[indx + BNXT_RE_EXP_RQ_SGE_OFFSET] :
			bnxt_re_get_hwqe(que, *idx);
		(*idx)++;
		/*
		 * Corrupt SGE PA at the desired RQ index if error flag is
		 * set
		 */
		if ((que->tail == bnxt_err_rq_idx) &&
		    (bnxt_dev_debug_mask & BNXT_ERR_INJECT_LOCAL_PROT))
			sge->pa = 0;
		else
			sge->pa = htole64(sgl[indx].addr);

		sge->lkey = htole32(sgl[indx].lkey);
		/*
		 * Corrupt SGE length at the desired RQ index if error flag is
		 * set
		 */
		if ((que->tail == bnxt_err_rq_idx) &&
		    (bnxt_dev_debug_mask & BNXT_ERR_INJECT_LOCAL_LEN))
			sge->length = 0xffffffff;
		else
			sge->length = htole32(sgl[indx].length);
	}
}

uint32_t bnxt_re_rq_hdr_val_test(struct bnxt_re_queue *rq, uint32_t opcd)
{
	/* use invalid opcode for error testing */
	if (rq->tail == bnxt_err_rq_idx && (bnxt_dev_debug_mask & BNXT_ERR_INJECT_OPCODE))
		return BNXT_RE_WR_OPCD_INVAL;
	return opcd;
}

uint32_t bnxt_re_sq_hdr_val_test(struct bnxt_re_queue *sq, uint8_t opcd)
{
	/* use invalid opcode for error testing */
	if (sq->tail == bnxt_err_sq_idx && (bnxt_dev_debug_mask & BNXT_ERR_INJECT_OPCODE))
		return BNXT_RE_WR_OPCD_INVAL;
	return opcd;
}

/* Dump the current wr */
void bnxt_dbg_dump_wr(struct ibv_send_wr *wr, struct bnxt_re_qp *qp, void *hdr, void *sqe)
{
	if (!(wr && (bnxt_dev_debug_mask & BNXT_DBG_DUMP_WR)))
		return;

	fprintf(stderr, "%s qpid %d qptype %d wr_id 0x%lx num_sge %d opcode %d\n", __func__,
		qp->qpid, qp->qptyp, wr->wr_id,  wr->num_sge, wr->opcode);
	fprintf(stderr, "%s sg_addr 0x%lx sg_len %d sg_lkey %d\n", __func__,
		wr->sg_list->addr, wr->sg_list->length, wr->sg_list->lkey);
	fprintf(stderr, "%s wqe_hdr_addr: 0x%lx wqe_sqe_addr: 0x%lx\n",
		__func__, (uint64_t)hdr, (uint64_t)sqe);
	if (qp->cctx->is_hsi_v3) {
		struct bnxt_re_bsqe_v3 *hdr_v3 = (struct bnxt_re_bsqe_v3 *)hdr;

		fprintf(stderr, "%s opaque: 0x%08x\n", __func__,
			hdr_v3->opaque);
	}

	switch (wr->opcode) {
	case IBV_WR_SEND_WITH_IMM:
	case IBV_WR_SEND:
		fprintf(stderr, "UD: remote_qpn %d remote_qkey %d\n", wr->wr.ud.remote_qpn,
			wr->wr.ud.remote_qkey);
		break;
	case IBV_WR_RDMA_WRITE_WITH_IMM:
	case IBV_WR_RDMA_WRITE:
	case IBV_WR_RDMA_READ:
		fprintf(stderr, "RDMA: remote_addr %lx rkey %x\n", wr->wr.rdma.remote_addr,
			wr->wr.rdma.rkey);
		break;
	case IBV_WR_ATOMIC_CMP_AND_SWP:
	case IBV_WR_ATOMIC_FETCH_AND_ADD:
		fprintf(stderr, "ATOMIC: remote_addr %lx compare_add %lx swap %lx rkey %x\n",
			wr->wr.atomic.remote_addr, wr->wr.atomic.compare_add,
			wr->wr.atomic.swap, wr->wr.atomic.rkey);
		break;
	default:
		fprintf(stderr, "NON wire wr\n");
		break;
	}
}

static void bnxt_dbg_cycle_cnt_print(struct bnxt_dbg_cycle_cnt *prof)
{
	uint32_t i, num_segs;

	num_segs = prof->num_steps - 1;
	fprintf(stderr, DEV "%s avg cycles/iter:", prof->label);
	for (i = 0; i < num_segs; i++)
		fprintf(stderr, " seg%d=%lu", i + 1,
			prof->cycles_elapsed[i] / prof->iter);
	fprintf(stderr, " (over %u iterations)\n", prof->iter);
}

/*
 * Moderation priority:
 *   1. per-context mod (if caller passes nonzero)
 *   2. BNXT_DBG_CYCLE_CNT_MOD env var (parsed at init)
 *   3. BNXT_DBG_CYCLE_CNT_DEFAULT_MOD (compile-time default, 10000)
 */
void bnxt_dbg_cycle_cnt_init(struct bnxt_dbg_cycle_cnt *prof,
			     uint32_t num_steps, uint32_t mod,
			     const char *label)
{
	if (num_steps < 2)
		num_steps = 2;
	if (num_steps > BNXT_DBG_CYCLE_CNT_MAX_STEPS)
		num_steps = BNXT_DBG_CYCLE_CNT_MAX_STEPS;
	if (mod == 0)
		mod = bnxt_dbg_cycle_cnt_mod;

	memset(prof, 0, sizeof(*prof));
	prof->num_steps = num_steps;
	prof->mod = mod;
	snprintf(prof->label, sizeof(prof->label), "%s", label);
}

void bnxt_dbg_cycle_cnt_step(struct bnxt_dbg_cycle_cnt *prof, int step)
{
	if (!(bnxt_dev_debug_mask & BNXT_DBG_DUMP_CYCLES))
		return;

	if (step < 0 || (uint32_t)step >= prof->num_steps)
		return;

	prof->cycles[step] = bnxt_dbg_read_cycles();
}

void bnxt_dbg_cycle_cnt_end(struct bnxt_dbg_cycle_cnt *prof)
{
	uint32_t i, num_segs;

	if (!(bnxt_dev_debug_mask & BNXT_DBG_DUMP_CYCLES))
		return;

	num_segs = prof->num_steps - 1;
	for (i = 0; i < num_segs; i++)
		prof->cycles_elapsed[i] += prof->cycles[i + 1] - prof->cycles[i];

	prof->iter++;
	if (prof->iter >= prof->mod) {
		bnxt_dbg_cycle_cnt_print(prof);
		prof->iter = 0;
		memset(prof->cycles_elapsed, 0, sizeof(prof->cycles_elapsed));
	}
}

/* dump the MSN entry */
void dump_dbg_msn_tbl(struct bnxt_re_qp *qp, struct bnxt_re_msns *msns,
		      uint32_t len, uint32_t st_idx)
{
	if (!(bnxt_dev_debug_mask & BNXT_DBG_DUMP_MSN))
		return;

	fprintf(stderr, "%s msnsp %p qpnum %d wqe_cnt %ld wqelen %d\n"
		"msn %d start_idx %u next_psn %u start_psn %u raw %lx\n", __func__,
		msns, qp->ibvqp->qp_num, qp->wqe_cnt, len, qp->jsqq->hwque->msn,
		st_idx,
		(uint32_t)le32toh(BNXT_RE_MSN_NPSN(msns->start_idx_next_psn_start_psn)),
		(uint32_t)le32toh(BNXT_RE_MSN_SPSN(msns->start_idx_next_psn_start_psn)),
		(uint64_t)(msns->start_idx_next_psn_start_psn));
}
