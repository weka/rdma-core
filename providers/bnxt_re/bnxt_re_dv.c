// SPDX-License-Identifier: GPL-2.0-only
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
 * Description: Direct verbs API function definitions.
 */

#include <stdio.h>
#include <sys/mman.h>

#include "main.h"
#include "bnxt_re-abi.h"
#include "bnxt_re_dv_abi.h"
#include "bnxt_re_dv.h"
#include "./verbs.h"
#include "bnxt_re_dv_internal.h"

static void bnxt_re_dv_get_qp(struct ibv_qp *qp_in, struct bnxt_re_dv_qp *qp_out)
{
	struct bnxt_re_qp *qp = to_bnxt_re_qp(qp_in);

	qp_out->wqe_cnt = qp->wqe_cnt;
}

static void bnxt_re_dv_get_pd(struct ibv_pd *pd_in, struct bnxt_re_dv_pd *pd_out)
{
	struct bnxt_re_pd *pd = to_bnxt_re_pd(pd_in);

	pd_out->pdn = pd->pdid;
}

static void bnxt_re_dv_get_cq(struct ibv_cq *cq_in, struct bnxt_re_dv_cq *cq_out)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(cq_in);

	cq_out->cqn = cq->cqid;
}

static void bnxt_re_dv_get_srq(struct ibv_srq *srq_in, struct bnxt_re_dv_srq *srq_out)
{
	struct bnxt_re_srq *srq = to_bnxt_re_srq(srq_in);

	srq_out->srqn = srq->srqid;
}

static void bnxt_re_dv_get_av(struct ibv_ah *ah_in, struct bnxt_re_dv_ah *ah_out)
{
	struct bnxt_re_ah *re_ah = to_bnxt_re_ah(ah_in);

	ah_out->comp_mask = 0;
	ah_out->avid = re_ah->avid;
}

int bnxt_re_dv_init_obj(struct bnxt_re_dv_obj *obj, uint64_t obj_type)
{
	if (!obj)
		return -EINVAL;

	if (obj_type & BNXT_RE_DV_OBJ_QP)
		bnxt_re_dv_get_qp(obj->qp.in, obj->qp.out);
	if (obj_type & BNXT_RE_DV_OBJ_PD)
		bnxt_re_dv_get_pd(obj->pd.in, obj->pd.out);
	if (obj_type & BNXT_RE_DV_OBJ_CQ)
		bnxt_re_dv_get_cq(obj->cq.in, obj->cq.out);
	if (obj_type & BNXT_RE_DV_OBJ_SRQ)
		bnxt_re_dv_get_srq(obj->srq.in, obj->srq.out);
	if (obj_type & BNXT_RE_DV_OBJ_AH)
		bnxt_re_dv_get_av(obj->ah.in, obj->ah.out);

	return 0;
}

/* Returns details about the default Doorbell page for ucontext */
int bnxt_re_dv_get_default_db_region(struct ibv_context *ibvctx,
				     struct bnxt_re_dv_db_region_attr *out)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dv_db_region_attr attr = {};
	int ret;

	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DBR,
			       BNXT_RE_METHOD_DBR_QUERY,
			       1);

	fill_attr_out_ptr(cmd, BNXT_RE_DV_QUERY_DBR_ATTR, &attr);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		return ret;
	}
	out->dbr = cntx->udpi.dbpage;
	out->dpi = attr.dpi;
	out->umdbr = attr.umdbr;
	return 0;
}

int bnxt_re_dv_free_db_region(struct ibv_context *ctx,
			      struct bnxt_re_dv_db_region_attr *attr)
{
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ctx->device);
	int ret;

	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DBR,
			       BNXT_RE_METHOD_DBR_FREE,
			       1);

	if (attr->dbr != MAP_FAILED)
		munmap(attr->dbr, dev->pg_size);

	bnxt_trace_dv(NULL, DEV "%s: DV DBR: handle: 0x%x\n", __func__, attr->handle);
	fill_attr_in_obj(cmd, BNXT_RE_DV_FREE_DBR_HANDLE, attr->handle);

	ret = execute_ioctl(ctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n",
			__func__, ret);
		errno = ret;
		return ret;
	}

	free(attr);
	return 0;
}

struct bnxt_re_dv_db_region_attr *
bnxt_re_dv_alloc_db_region(struct ibv_context *ctx)
{
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ctx->device);
	struct bnxt_re_dv_db_region_attr attr = {}, *out;
	struct ib_uverbs_attr *handle;
	uint64_t mmap_offset = 0;
	int ret;

	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DBR,
			       BNXT_RE_METHOD_DBR_ALLOC,
			       3);

	out = calloc(1, sizeof(*out));
	if (!out) {
		errno = ENOMEM;
		return NULL;
	}

	handle = fill_attr_out_obj(cmd, BNXT_RE_DV_ALLOC_DBR_HANDLE);
	fill_attr_out_ptr(cmd, BNXT_RE_DV_ALLOC_DBR_ATTR, &attr);
	fill_attr_out_ptr(cmd, BNXT_RE_DV_ALLOC_DBR_OFFSET, &mmap_offset);

	ret = execute_ioctl(ctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n",
			__func__, ret);
		free(out);
		errno = ret;
		return NULL;
	}
	out->handle = read_attr_obj(BNXT_RE_DV_ALLOC_DBR_HANDLE, handle);
	out->dpi = attr.dpi;
	out->umdbr = attr.umdbr;

	out->dbr = mmap(NULL, dev->pg_size, PROT_WRITE,
			MAP_SHARED, ctx->cmd_fd, mmap_offset);
	if (out->dbr == MAP_FAILED) {
		fprintf(stderr, DEV "%s: mmap failed\n", __func__);
		bnxt_re_dv_free_db_region(ctx, out);
		errno = ENOMEM;
		return NULL;
	}
	bnxt_trace_dv(NULL, "%s: DV DBR: handle: 0x%x\n", __func__, out->handle);

	return out;
}

int
bnxt_re_dv_send_fw_msg(struct ibv_context *ctx, void *in, int in_size,
		       void *out, int out_size)
{
	int ret;

	DECLARE_COMMAND_BUFFER(cmd, BNXT_RE_OBJECT_DV_SEND_FW_MSG,
			       BNXT_RE_METHOD_DV_SEND_FW_MSG, 2);

	fill_attr_in(cmd, BNXT_RE_SEND_FW_MSG_IN, in, in_size);
	fill_attr_out(cmd, BNXT_RE_SEND_FW_MSG_OUT, out, out_size);

	ret = execute_ioctl(ctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n",
			__func__, ret);
		errno = ret;
	}
	return ret;
}

void *bnxt_re_dv_umem_reg(struct ibv_context *ibvctx, struct bnxt_re_dv_umem_reg_attr *in)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_UMEM,
			       BNXT_RE_METHOD_UMEM_REG,
			       9);
	struct ib_uverbs_attr *handle;
	struct bnxt_re_dv_umem *umem;
	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem) {
		errno = ENOMEM;
		return NULL;
	}
	if (ibv_dontfork_range(in->addr, in->size))
		goto err;

	fill_attr_in_uint64(cmd, BNXT_RE_UMEM_OBJ_REG_ADDR, (uintptr_t)in->addr);
	fill_attr_in_uint64(cmd, BNXT_RE_UMEM_OBJ_REG_LEN, in->size);
	fill_attr_in_uint32(cmd, BNXT_RE_UMEM_OBJ_REG_ACCESS, in->access_flags);
	if (in->comp_mask & BNXT_RE_DV_UMEM_FLAGS_DMABUF) {
		if (in->dmabuf_fd == -1) {
			fprintf(stderr, "%s: failed: EBADF\n", __func__);
			errno = EBADF;
			goto err;
		}
		fill_attr_in_fd(cmd, BNXT_RE_UMEM_OBJ_REG_DMABUF_FD,
				in->dmabuf_fd);
	}

	if (in->comp_mask & BNXT_RE_DV_UMEM_FLAGS_MAP) {
		if (!in->pa_buf || (in->pa_buf_sz < sizeof(uint64_t))) {
			errno = EINVAL;
			goto err;
		}
		fill_attr_in_uint64(cmd, BNXT_RE_UMEM_OBJ_REG_PA_ARR, (uintptr_t)in->pa_buf);
		fill_attr_in_uint64(cmd, BNXT_RE_UMEM_OBJ_REG_PA_ARR_LEN, in->pa_buf_sz);
		fill_attr_out_ptr(cmd, BNXT_RE_UMEM_OBJ_REG_MAP_PG_SZ, &in->map_pg_sz);
	}

	fill_attr_in_uint64(cmd, BNXT_RE_UMEM_OBJ_REG_PGSZ_BITMAP,
			    in->pgsz_bitmap);
	handle = fill_attr_out_obj(cmd, BNXT_RE_UMEM_OBJ_REG_HANDLE);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		goto err_umem_reg_cmd;
	}

	umem->handle = read_attr_obj(BNXT_RE_UMEM_OBJ_REG_HANDLE, handle);
	umem->context = ibvctx;
	umem->addr = in->addr;
	umem->size = in->size;

	bnxt_trace_dv(NULL, "%s: DV Umem Reg: handle: 0x%x addr: 0x%lx size: 0x%lx\n",
		      __func__, umem->handle, (uint64_t)umem->addr, umem->size);
	return (void *)umem;
err_umem_reg_cmd:
	ibv_dofork_range(in->addr, in->size);
err:
	free(umem);
	return NULL;
}

int bnxt_re_dv_umem_dereg(void *umem_handle)
{
	struct bnxt_re_dv_umem *umem = umem_handle;

	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_UMEM,
			       BNXT_RE_METHOD_UMEM_DEREG,
			       1);
	int ret;

	bnxt_trace_dv(NULL, "%s: DV Umem Dereg: handle: 0x%x\n",
		      __func__, umem->handle);
	fill_attr_in_obj(cmd, BNXT_RE_UMEM_OBJ_DEREG_HANDLE, umem->handle);
	ret = execute_ioctl(umem->context, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n",
			__func__, ret);
		return ret;
	}

	ibv_dofork_range(umem->addr, umem->size);
	free(umem);
	return 0;
}

static struct ibv_context *bnxt_re_to_ibvctx(struct bnxt_re_context *cntx)
{
#ifdef RCP_USE_IB_UVERBS
	return &cntx->ibvctx.context;
#else
	return &cntx->ibvctx;
#endif
}

static bool bnxt_re_dv_is_valid_umem(struct bnxt_re_dev *dev, struct bnxt_re_dv_umem *umem,
				     uint64_t offset, uint32_t size)
{
	return ((offset == get_aligned(offset, dev->pg_size)) &&
		(offset + size <= umem->size));
}

/* Apps can use the helper function below to allocate
 * CQ memory. The flag below to indicate this mode
 * is set if this helper is used. Otherwise, it is
 * assumed that the app allocates and manages CQ memory.
 *
 * Note that if the helper is used, the library can
 * handle the datapath (post_send, poll) also; otherwise
 * the app needs to have its own datapath functions.
 */
bool bnxt_re_dv_cq_alloc_helper;

void *bnxt_re_dv_cq_mem_alloc(struct ibv_context *ibvctx, int num_cqe,
			      struct bnxt_re_dv_cq_attr *cq_attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvctx->device);
	struct bnxt_re_cq *cq;

	if (num_cqe > dev->max_cq_depth)
		return NULL;

	cq = calloc(1, (sizeof(*cq) + sizeof(struct bnxt_re_queue)));
	if (!cq)
		return NULL;

	cq->cqq = (void *)((char *)cq + sizeof(*cq));
	cq->mem = bnxt_re_alloc_cqslab(cntx, num_cqe, 0);
	if (!cq->mem)
		goto mem;

	cq->cqq->depth = cq->mem->pad;
	cq->cqq->stride = bnxt_re_get_cqe_sz();
	cq->cqq->va = cq->mem->va_head;
	if (!cq->cqq->va)
		goto fail;

	cq_attr->cqe_size = cq->cqq->stride;
	cq_attr->ncqe = cq->cqq->depth;
	bnxt_trace_dv(NULL, "%s: Updating ncqe from:%d to:%d\n",
		      __func__, num_cqe, cq_attr->ncqe);

	cq->verbs_cq.cq.context = bnxt_re_to_ibvctx(cntx);
	bnxt_re_dv_cq_alloc_helper = true;
	return cq;

fail:
	bnxt_re_free_mem(cq->mem, NULL);
mem:
	free(cq);
	return NULL;
}

static int bnxt_re_dv_create_cq_cmd(struct bnxt_re_dev *dev,
				    struct ibv_context *ibvctx,
				    struct bnxt_re_cq *cq,
				    struct bnxt_re_dv_cq_init_attr *cq_attr,
				    uint64_t comp_mask,
				    struct bnxt_re_dv_cq_resp *resp)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_CQ,
			       BNXT_RE_METHOD_DV_CREATE_CQ,
			       5);
	struct bnxt_re_dv_umem *cq_umem = cq->cq_umem;
	uint64_t offset = cq_attr->cq_umem_offset;
	struct bnxt_re_dv_cq_req req = {};
	struct ib_uverbs_attr *handle;
	uint32_t size;
	int ret;

	/* Input args */
	req.ncqe = cq_attr->ncqe;
	req.va = (uint64_t)cq->cqq->va;
	req.comp_mask = comp_mask;
	fill_attr_in_ptr(cmd, BNXT_RE_DV_CREATE_CQ_REQ, &req);

	size = cq_attr->ncqe * bnxt_re_get_cqe_sz();
	if (!bnxt_re_dv_is_valid_umem(dev, cq_umem, offset, size)) {
		fprintf(stderr,
			"%s: Invalid cq_umem: handle: 0x%x offset: 0x%lx size: 0x%x\n",
			__func__, cq_umem->handle, offset, size);
		return -EINVAL;
	}
	fill_attr_in_uint64(cmd, BNXT_RE_DV_CREATE_CQ_UMEM_OFFSET, offset);

	if (cq_umem) {
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_CQ_UMEM_HANDLE,
				 cq_umem->handle);
		bnxt_trace_dv(NULL,
			      "%s: cq_umem: handle: 0x%x offset: 0x%lx size: 0x%x\n",
			      __func__, cq_umem->handle, offset, size);
	}

	/* Output args */
	handle = fill_attr_out_obj(cmd, BNXT_RE_DV_CREATE_CQ_HANDLE);
	fill_attr_out_ptr(cmd, BNXT_RE_DV_CREATE_CQ_RESP, resp);

	bnxt_trace_dv(NULL, "%s: ncqe: %d va: 0x%" PRIx64 " comp_mask: 0x%" PRIx64 "\n",
		      __func__, req.ncqe, (uint64_t)req.va, (uint64_t)req.comp_mask);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		return ret;
	}
	cq->verbs_cq.cq.handle = read_attr_obj(BNXT_RE_DV_CREATE_CQ_HANDLE, handle);

	bnxt_trace_dv(NULL, "%s: CQ handle: 0x%x\n", __func__, cq->verbs_cq.cq.handle);
	bnxt_trace_dv(NULL,
		      "%s: CQ cqid: 0x%x tail: 0x%x phase: 0x%x comp_mask: 0x%llx\n",
		      __func__, resp->cqid, resp->tail, resp->phase, resp->comp_mask);

	return 0;
}

static int bnxt_re_dv_init_cq(struct ibv_context *ibvctx, struct bnxt_re_cq *cq,
			      struct bnxt_re_dv_cq_resp *resp)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_mmap_info minfo = {};
	int ret = 0;

	cq->cqid = resp->cqid;
	cq->phase = resp->phase;
	cq->cqq->tail = resp->tail;
	cq->udpi = &cntx->udpi;
	cq->first_arm = true;
	cq->cntx = cntx;
	cq->rand.seed = cq->cqid;
	cq->shadow_db_key = BNXT_RE_DB_KEY_INVALID;
	if (!(resp->comp_mask & BNXT_RE_CQ_TOGGLE_PAGE_SUPPORT))
		goto done;

	minfo.type = BNXT_RE_CQ_TOGGLE_MEM;
	minfo.res_id = resp->cqid;
	ret = bnxt_re_get_toggle_mem(ibvctx, &minfo, NULL);
	if (ret) {
		fprintf(stderr, "%s: bnxt_re_get_toggle_mem() failed: %d\n",
			__func__, ret);
		return ret;
	}
	cq->toggle_map = mmap(NULL, minfo.alloc_size, PROT_READ,
			      MAP_SHARED, ibvctx->cmd_fd,
			      minfo.alloc_offset);
	if (cq->toggle_map == MAP_FAILED) {
		fprintf(stderr, "%s: mmap() failed\n", __func__);
		cq->toggle_map = NULL;
		ret = -EIO;
		return ret;
	}
	cq->toggle_size = minfo.alloc_size;

	bnxt_trace_dv(NULL, "%s: toggle_map: 0x%lx toggle_size: %d\n",
		      __func__, (uintptr_t)cq->toggle_map, cq->toggle_size);

done:
	bnxt_re_dp_spin_init(&cq->cqq->qlock, PTHREAD_PROCESS_PRIVATE, !bnxt_single_threaded);
	INIT_DBLY_LIST_HEAD(&cq->sfhead);
	INIT_DBLY_LIST_HEAD(&cq->rfhead);
	INIT_DBLY_LIST_HEAD(&cq->prev_cq_head);
	return ret;
}

void *bnxt_re_dv_cq_umem_reg(struct ibv_context *ibvctx, struct bnxt_re_cq *cq)
{
	struct bnxt_re_dv_umem_reg_attr in = {};

	in.addr = cq->cqq->va;
	in.size = cq->cqq->depth * 32;
	in.access_flags = IBV_ACCESS_LOCAL_WRITE;

	return bnxt_re_dv_umem_reg(ibvctx, &in);
}

/**
 * Create L2 CQ via standard ibv_cmd_create_cq() path
 * This is similar to _bnxt_re_create_cq() but uses UMEM-provided
 * memory and sets L2 flag in comp_mask
 */
static struct ibv_cq *
bnxt_re_dv_create_l2_cq(struct ibv_context *ibvctx,
			struct bnxt_re_dv_cq_init_attr *cq_attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvctx->device);
	struct ibv_cq_init_attr_ex attr_ex = {};
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	struct bnxt_re_cq_resp_ex resp = {};
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	struct bnxt_re_cq_req_ex cmd = {};
	struct bnxt_re_dv_umem *umem;
	struct bnxt_re_cq *cq;
	void *cq_va;
	int ret;

	if (cq_attr->ncqe > dev->max_cq_depth)
		return NULL;

	cq = calloc(1, (sizeof(*cq) + sizeof(struct bnxt_re_queue)));
	if (!cq)
		return NULL;

	cq->cqq = (void *)((char *)cq + sizeof(*cq));
	cq->cqq->depth = cq_attr->ncqe;
	cq->cqq->stride = bnxt_re_get_cqe_sz();

	/* Get CQ VA from UMEM */
	if (!cq_attr->umem_handle) {
		fprintf(stderr, "%s: UMEM handle required for L2 CQ\n", __func__);
		goto fail;
	}

	umem = (struct bnxt_re_dv_umem *)cq_attr->umem_handle;
	cq_va = (void *)(umem->addr + cq_attr->cq_umem_offset);
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	cq->cqq->va = cq_va;

	/* Prepare command structure */
	memset(&cmd, 0, sizeof(cmd));
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	cmd.cq_va = (uint64_t)cq_va;
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	cmd.cq_handle = (uint64_t)cq;
	cmd.comp_mask = BNXT_RE_COMP_MASK_CQ_REQ_L2;  /* L2-specific flag */
	cmd.comp_mask |= BNXT_RE_COMP_MASK_CQ_REQ_HAS_HDBR_KADDR;
	/* coverity[UNUSED_VALUE:SUPPRESS] */
	cmd.comp_mask |= BNXT_RE_COMP_MASK_CQ_REQ_IGNORE_OVERRUN;
	attr_ex.cqe = cq_attr->ncqe;
	ret = ibv_cmd_create_cq_ex_compat(ibvctx, &attr_ex,
					  &cq->verbs_cq, &cmd.cmd,
					  sizeof(cmd), &resp.resp,
					  sizeof(resp), 0);
	if (ret) {
		fprintf(stderr, "%s: ibv_cmd_create_cq() failed: %d\n", __func__, ret);
		goto fail;
	} else {
		bnxt_trace_dv(NULL, "%s: Created CQ: cqid: %d\n", __func__, resp.cqid);
	}

	/* Initialize CQ from response */
	cq->cqid = resp.cqid;
	cq->phase = resp.phase;
	cq->cqq->tail = resp.tail;
	cq->udpi = &cntx->udpi;
	cq->first_arm = true;
	cq->cntx = cntx;
	cq->rand.seed = cq->cqid;
	cq->shadow_db_key = BNXT_RE_DB_KEY_INVALID;

	if (resp.comp_mask & BNXT_RE_CQ_TOGGLE_PAGE_SUPPORT) {
		struct bnxt_re_mmap_info minfo = {};

		minfo.type = BNXT_RE_CQ_TOGGLE_MEM;
		minfo.res_id = resp.cqid;
		ret = bnxt_re_get_toggle_mem(ibvctx, &minfo, NULL);
		if (!ret) {
			cq->toggle_map = mmap(NULL, minfo.alloc_size, PROT_READ,
					      MAP_SHARED, ibvctx->cmd_fd,
					      minfo.alloc_offset);
			if (cq->toggle_map != MAP_FAILED)
				cq->toggle_size = minfo.alloc_size;
		}
	}
	/* Handle HDBR
	 * Note: we skip HDBR for L2 CQs created via ibv_cmd.
	 * This is acceptable as L2 CQs typically don't use HDBR.
	 */
	if (resp.comp_mask & BNXT_RE_CQ_HDBR_KADDR_SUPPORT) {
		/* HDBR not supported for L2 CQs via ibv_cmd path */
		/* cq->dbc would be set here if needed */
	}

	/* Initialize spinlock and lists */
	bnxt_re_dp_spin_init(&cq->cqq->qlock, PTHREAD_PROCESS_PRIVATE,
			     !bnxt_single_threaded);
	INIT_DBLY_LIST_HEAD(&cq->sfhead);
	INIT_DBLY_LIST_HEAD(&cq->rfhead);
	INIT_DBLY_LIST_HEAD(&cq->prev_cq_head);

	/* Store UMEM handle for cleanup */
	cq->cq_umem = (struct bnxt_re_dv_umem *)cq_attr->umem_handle;
	cq->dv_cq_flags = BNXT_DV_CQ_FLAGS_VALID | BNXT_DV_CQ_FLAGS_L2;

	cq_attr->cqid = cq->cqid;
	return &cq->verbs_cq.cq;

fail:
	free(cq);
	return NULL;
}

struct ibv_cq *bnxt_re_dv_create_cq(struct ibv_context *ibvctx,
				    struct bnxt_re_dv_cq_init_attr *cq_attr)
{
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvctx->device);
	struct bnxt_re_dv_umem *cq_umem;
	uint64_t comp_mask = cq_attr->comp_mask;
	struct bnxt_re_dv_cq_resp resp = {};
	struct bnxt_re_cq *cq;
	void *new_va;
	int ret;

	/* Route L2 CQ through standard ibv_cmd_create_cq() path
	 * Check if comp_mask has L2 flag set (DV API flag)
	 */
	if (comp_mask & BNXT_RE_DV_CQ_FLAG_L2)
		return bnxt_re_dv_create_l2_cq(ibvctx, cq_attr);

	/* Existing Direct Verbs path for RoCE CQs */
	cq_umem = (struct bnxt_re_dv_umem *)cq_attr->umem_handle;

	/* Existing Direct Verbs path for RoCE CQs */
	if (!bnxt_re_dv_cq_alloc_helper) {
		if (cq_attr->ncqe > dev->max_cq_depth)
			return NULL;

		cq = calloc(1, (sizeof(*cq) + sizeof(struct bnxt_re_queue)));
		if (!cq)
			return NULL;

		cq->cqq = (void *)((char *)cq + sizeof(*cq));
		cq->cqq->depth = cq_attr->ncqe;
		cq->cqq->stride = bnxt_re_get_cqe_sz();
	} else {
		cq = (struct bnxt_re_cq *)cq_attr->cq_handle;
		cq->dv_cq_flags = BNXT_DV_CQ_FLAGS_HELPER;
	}

	new_va = cq_umem->addr + cq_attr->cq_umem_offset;
	bnxt_trace_dv(NULL, "%s: Updating CQ VA from: 0x%lx to: 0x%lx\n",
		      __func__, (uint64_t)cq->cqq->va, (uint64_t)new_va);
	cq->cqq->va = new_va;

	if (!cq_attr->umem_handle) {
		cq_umem = bnxt_re_dv_cq_umem_reg(ibvctx, cq);
		if (!cq_umem) {
			fprintf(stderr, "%s: bnxt_re_dv_cq_umem_reg() failed\n", __func__);
			goto fail;
		}
		cq->dv_cq_flags |= BNXT_DV_CQ_FLAGS_UMEM_REG_DEFAULT;
	}
	cq->cq_umem = cq_umem;

	ret = bnxt_re_dv_create_cq_cmd(dev, ibvctx, cq, cq_attr, comp_mask, &resp);
	if (ret) {
		fprintf(stderr, "%s: bnxt_re_dv_create_cq_cmd() failed\n", __func__);
		goto umem_dereg;
	}

	ret = bnxt_re_dv_init_cq(ibvctx, cq, &resp);
	if (ret) {
		fprintf(stderr, "%s: bnxt_re_dv_create_cq_cmd() failed\n", __func__);
		goto umem_dereg;
	}

	cq->dv_cq_flags |= BNXT_DV_CQ_FLAGS_VALID;
	return &cq->verbs_cq.cq;

umem_dereg:
	if (cq->dv_cq_flags & BNXT_DV_CQ_FLAGS_UMEM_REG_DEFAULT)
		bnxt_re_dv_umem_dereg(cq->cq_umem);
fail:
	if (cq->dv_cq_flags & BNXT_DV_CQ_FLAGS_HELPER)
		bnxt_re_free_mem(cq->mem, NULL);
	free(cq);
	return NULL;
}

int bnxt_re_dv_destroy_cq(struct ibv_cq *ibvcq)
{
	struct bnxt_re_cq *cq = to_bnxt_re_cq(ibvcq);
	int ret;

	/* If created via ibv_cmd, use standard destroy path */
	if (cq->dv_cq_flags & BNXT_DV_CQ_FLAGS_L2) {
		ret = ibv_cmd_destroy_cq(ibvcq);
		if (ret)
			return ret;

		/* Cleanup toggle map if present */
		if (cq->toggle_map)
			munmap(cq->toggle_map, cq->toggle_size);

		free(cq);
		return 0;
	}

	/* Existing Direct Verbs destruction path */
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_CQ,
			       BNXT_RE_METHOD_DV_DESTROY_CQ,
			       1);
	struct ibv_context *ibvctx = bnxt_re_to_ibvctx(cq->cntx);

	fill_attr_in_obj(cmd, BNXT_RE_DV_DESTROY_CQ_HANDLE, ibvcq->handle);
	bnxt_trace_dv(NULL, "%s: CQ handle: 0x%x\n", __func__, ibvcq->handle);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		return ret;
	}

	if (cq->umem_reg) {
		bnxt_re_dv_umem_dereg(cq->cq_umem);
		bnxt_re_free_mem(cq->mem, NULL);
	}

	if (cq->toggle_map)
		munmap(cq->toggle_map, cq->toggle_size);
	free(cq);
	return ret;
}

static struct bnxt_re_qp *
bnxt_re_dv_alloc_qp(struct ibv_context *ibvctx,
		    struct ibv_qp_init_attr_ex *attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_qattr qattr[2];
	struct bnxt_re_qp *qp;
	void *mem;

	if (bnxt_re_check_qp_limits(cntx, attr))
		return NULL;

	memset(qattr, 0, (2 * sizeof(*qattr)));
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
	qp->qpmode = cntx->modes & BNXT_RE_WQE_MODE_VARIABLE;
	qp->re_pd = to_bnxt_re_pd(attr->pd);

	/* alloc queue pointers */
	if (bnxt_re_alloc_queue_ptr(qp, attr))
		goto fail;

	/* alloc queues */
	if (bnxt_re_alloc_queues(qp, attr, qattr))
		goto fail;

	bnxt_trace_dv(NULL,
		      "%s: sq_va: 0x%" PRIx64 " sq_len: 0x%x rq_va: 0x%" PRIx64 " rq_len: 0x%x\n",
		      __func__, (uint64_t)qp->jsqq->hwque->va, qattr[0].sz_ring,
		      (qp->jrqq ? (uint64_t)qp->jrqq->hwque->va : 0), qattr[1].sz_ring);
	memcpy(qp->qattr, qattr, sizeof(qattr));
	return qp;
fail:
	bnxt_re_free_mem(mem, NULL);
	return NULL;
}

static int
bnxt_re_dv_create_qp_cmd_int(struct ibv_context *ibvctx,
			     struct bnxt_re_dv_qp_init_attr_internal *dv_qp_attr_int,
			     struct bnxt_re_dv_qp_init_attr *dv_qp_attr,
			     struct bnxt_re_dv_create_qp_resp *resp,
			     struct bnxt_re_qp *qp)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_QP,
			       BNXT_RE_METHOD_DV_CREATE_QP,
			       9);
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dv_db_region_attr *db_attr = NULL;
	struct bnxt_re_dv_create_qp_req req = {};
	struct ib_uverbs_attr *handle;
	struct bnxt_re_cq *re_cq;
	int ret;

	/* Input args */
	req.qp_type = dv_qp_attr->qp_type;
	req.max_send_wr = dv_qp_attr->max_send_wr;
	req.max_recv_wr = dv_qp_attr->max_recv_wr;
	req.max_send_sge = dv_qp_attr->max_send_sge;
	req.max_recv_sge = dv_qp_attr->max_recv_sge;
	req.max_inline_data = dv_qp_attr->max_inline_data;

	req.pd_id = dv_qp_attr_int->pdid;
	req.qp_handle = dv_qp_attr_int->qp_handle;
	if (qp->sq_umem) {
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_SQ_UMEM_HANDLE,
				 qp->sq_umem->handle);
		req.sq_umem_offset = 0;
		req.sq_va = 0;
	} else {
		req.sq_va = dv_qp_attr_int->sq_va;
	}
	req.sq_len = dv_qp_attr_int->sq_len;
	req.sq_slots = dv_qp_attr_int->sq_slots;
	req.sq_wqe_sz = dv_qp_attr_int->sq_wqe_sz;
	req.sq_psn_sz = dv_qp_attr_int->sq_psn_sz;
	req.sq_npsn = dv_qp_attr_int->sq_npsn;

	if (!dv_qp_attr->srq) {
		if (qp->rq_umem) {
			fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_RQ_UMEM_HANDLE,
					 qp->rq_umem->handle);
			req.rq_umem_offset = 0;
			req.rq_va = 0;
		} else {
			req.rq_va = dv_qp_attr_int->rq_va;
		}
		req.rq_len = dv_qp_attr_int->rq_len;
		req.rq_va = dv_qp_attr_int->rq_va;
		req.rq_len = dv_qp_attr_int->rq_len;
		req.rq_slots = dv_qp_attr_int->rq_slots;
		req.rq_wqe_sz = dv_qp_attr_int->rq_wqe_sz;
	} else {
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_SRQ_HANDLE,
				 dv_qp_attr->srq->handle);
	}

	fill_attr_in_ptr(cmd, BNXT_RE_DV_CREATE_QP_REQ, &req);

	re_cq = to_bnxt_re_cq(dv_qp_attr->send_cq);
	fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_SEND_CQ_HANDLE,
			 re_cq->verbs_cq.cq.handle);

	re_cq = to_bnxt_re_cq(dv_qp_attr->recv_cq);
	fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_RECV_CQ_HANDLE,
			 re_cq->verbs_cq.cq.handle);

	if (dv_qp_attr->dbr_handle) {
		db_attr = dv_qp_attr->dbr_handle;
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_DBR_HANDLE,
				 db_attr->handle);
		qp->dv_dpi.dbpage = db_attr->dbr;
		qp->dv_dpi.dpindx = db_attr->dpi;
		qp->udpi = &qp->dv_dpi;
	} else {
		qp->udpi = &cntx->udpi;
	}

	/* Output args */
	handle = fill_attr_out_obj(cmd, BNXT_RE_DV_CREATE_QP_HANDLE);
	fill_attr_out_ptr(cmd, BNXT_RE_DV_CREATE_QP_RESP, resp);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		return ret;
	}

	qp->qp_handle = read_attr_obj(BNXT_RE_DV_CREATE_QP_HANDLE, handle);
	bnxt_trace_dv(NULL, "%s: QP handle: 0x%x qpid: 0x%x\n",
		      __func__, qp->qp_handle, resp->qpid);

	return 0;
}

static void bnxt_re_get_dv_qp_attr(struct bnxt_re_qp *qp, int indx,
				   struct bnxt_re_dv_qp_init_attr_internal *dv_qp_attr)
{
	struct bnxt_re_joint_queue *jqq;
	struct bnxt_re_queue *que;

	if (indx == BNXT_RE_QATTR_SQ_INDX) {
		jqq = qp->jsqq;
		que = jqq->hwque;

		dv_qp_attr->sq_va = (uint64_t)que->va;
		dv_qp_attr->sq_slots = que->depth;
		dv_qp_attr->sq_len = qp->qattr[indx].sz_ring;
		dv_qp_attr->sq_wqe_sz = qp->qattr[indx].esize;
		dv_qp_attr->sq_psn_sz = qp->qattr[indx].psn_sz;
		dv_qp_attr->sq_npsn = qp->qattr[indx].npsn;
	} else {
		jqq = qp->jrqq;
		if (jqq) {
			que = jqq->hwque;

			dv_qp_attr->rq_va = (uint64_t)que->va;
			dv_qp_attr->rq_slots = que->depth;
			dv_qp_attr->rq_len = qp->qattr[indx].sz_ring;
			dv_qp_attr->rq_wqe_sz = qp->qattr[indx].esize;
		}
	}
}

static void bnxt_re_print_dv_qp_attr(struct ibv_qp_init_attr_ex *attr,
				     struct bnxt_re_dv_qp_init_attr_internal *ap)
{
	struct bnxt_re_cq *cq;

	if (!(bnxt_debug_mask & BNXT_DUMP_DV))
		return;

	fprintf(stderr, "DV_QP_ATTR:\n");
	fprintf(stderr,
		"\t qp_type: 0x%x pdid: 0x%x qp_handle: 0x%" PRIx64 "\n",
		attr->qp_type, ap->pdid, ap->qp_handle);

	fprintf(stderr, "\t SQ ATTR:\n");
	fprintf(stderr,
		"\t\t max_send_wr: 0x%x max_send_sge: 0x%x\n",
		attr->cap.max_send_wr, attr->cap.max_send_sge);
	fprintf(stderr, "\t\t va: 0x%" PRIx64 " len: 0x%x slots: 0x%x wqe_sz: 0x%x\n",
		ap->sq_va, ap->sq_len, ap->sq_slots, ap->sq_wqe_sz);
	fprintf(stderr, "\t\t psn_sz: 0x%x npsn: 0x%x\n",
		ap->sq_psn_sz, ap->sq_npsn);
	cq = to_bnxt_re_cq(attr->send_cq);
	fprintf(stderr, "\t\t send_cq: handle: 0x%x id: 0x%x\n",
		attr->send_cq->handle, cq->cqid);

	fprintf(stderr, "\t RQ ATTR:\n");
	fprintf(stderr,
		"\t\t max_recv_wr: 0x%x max_recv_sge: 0x%x\n",
		attr->cap.max_recv_wr, attr->cap.max_recv_sge);
	fprintf(stderr, "\t\t va: 0x%" PRIx64 " len: 0x%x slots: 0x%x wqe_sz: 0x%x\n",
		ap->rq_va, ap->rq_len, ap->rq_slots, ap->rq_wqe_sz);
	cq = to_bnxt_re_cq(attr->recv_cq);
	fprintf(stderr, "\t\t recv_cq: handle: 0x%x id: 0x%x\n",
		attr->recv_cq->handle, cq->cqid);

	if (attr->srq)
		fprintf(stderr, "\t SRQ: handle: %u\n", attr->srq->handle);
}

/* Init some members of ibvqp for now; this may
 * not be needed in the final DV implementation.
 * Reference code: libibverbs/cmd_qp.c::set_qp()
 */
static void bnxt_re_dv_init_ib_qp(struct ibv_context *ibvctx,
				  struct ibv_qp_init_attr_ex *attr,
				  struct bnxt_re_qp *qp)
{
	struct ibv_qp *ibvqp = qp->ibvqp;

	ibvqp->handle =	qp->qp_handle;
	ibvqp->qp_num =	qp->qpid;
	ibvqp->context = ibvctx;
	ibvqp->qp_context = attr->qp_context;
	ibvqp->pd = attr->pd;
	ibvqp->send_cq = attr->send_cq;
	ibvqp->recv_cq = attr->recv_cq;
	ibvqp->srq = attr->srq;
	ibvqp->qp_type = attr->qp_type;
	ibvqp->state = IBV_QPS_RESET;
	ibvqp->events_completed = 0;
	pthread_mutex_init(&ibvqp->mutex, NULL);
	pthread_cond_init(&ibvqp->cond, NULL);
}

static void bnxt_re_dv_init_qp(struct ibv_context *ibvctx,
			       struct ibv_qp_init_attr_ex *attr,
			       struct bnxt_re_qp *qp,
			       struct bnxt_re_dv_create_qp_resp *resp)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct ibv_device_attr *devattr;
	struct bnxt_re_qpcap *cap;
	struct bnxt_re_dev *rdev;

	qp->qpid = resp->qpid;
	qp->qptyp = attr->qp_type;
	qp->qpst = IBV_QPS_RESET;
	qp->scq = to_bnxt_re_cq(attr->send_cq);
	qp->rcq = to_bnxt_re_cq(attr->recv_cq);
	if (attr->srq)
		qp->srq = to_bnxt_re_srq(attr->srq);
	qp->rand.seed = qp->qpid;
	qp->sq_shadow_db_key = BNXT_RE_DB_KEY_INVALID;
	qp->rq_shadow_db_key = BNXT_RE_DB_KEY_INVALID;
	qp->sq_msn = 0;

	rdev = cntx->rdev;
	devattr = &rdev->devattr;
	cap = &qp->cap;
	cap->max_ssge = attr->cap.max_send_sge;
	cap->max_rsge = attr->cap.max_recv_sge;
	cap->max_inline = attr->cap.max_inline_data;
	cap->sqsig = attr->sq_sig_all;
	cap->is_atomic_cap = devattr->atomic_cap;
	INIT_DBLY_LIST_NODE(&qp->snode);
	INIT_DBLY_LIST_NODE(&qp->rnode);
	INIT_DBLY_LIST_NODE(&qp->dbnode);

	bnxt_re_dv_init_ib_qp(ibvctx, attr, qp);
}

static void fill_ib_attr_from_dv_qp_attr(struct bnxt_re_dv_qp_init_attr *dv_qp_attr,
					 struct ibv_qp_init_attr *attr)
{
	attr->send_cq = dv_qp_attr->send_cq;
	attr->recv_cq = dv_qp_attr->recv_cq;
	attr->srq = dv_qp_attr->srq;
	attr->cap.max_send_wr = dv_qp_attr->max_send_wr;
	attr->cap.max_send_sge = dv_qp_attr->max_send_sge;
	attr->qp_type =  dv_qp_attr->qp_type;
	attr->cap.max_inline_data =  dv_qp_attr->max_inline_data;
	attr->cap.max_recv_wr =  dv_qp_attr->max_recv_wr;
	attr->cap.max_recv_sge =  dv_qp_attr->max_recv_sge;
}

static int bnxt_re_dv_qp_umem_reg(struct ibv_context *ibvctx,
				  struct bnxt_re_qp *qp,
				  struct bnxt_re_dv_qp_init_attr_internal
					*dv_qp_attr_int)
{
	struct bnxt_re_dv_umem_reg_attr umem_input;
	struct bnxt_re_dv_umem *umem;

	umem_input.addr = (void *)dv_qp_attr_int->sq_va;
	umem_input.size = dv_qp_attr_int->sq_len;
	umem_input.access_flags = IBV_ACCESS_LOCAL_WRITE;
	umem_input.comp_mask = 0;
	umem_input.pgsz_bitmap = 0;
	umem = bnxt_re_dv_umem_reg(ibvctx, &umem_input);
	if (!umem) {
		fprintf(stderr, "%s: SQ umem_reg() failed: %d\n", __func__, errno);
		goto fail;
	}
	qp->sq_umem = umem;

	if (!qp->jrqq) /* SRQ */
		return 0;

	umem_input.addr = (void *)dv_qp_attr_int->rq_va;
	umem_input.size = dv_qp_attr_int->rq_len;
	umem_input.access_flags = IBV_ACCESS_LOCAL_WRITE;
	umem_input.comp_mask = 0;
	umem_input.pgsz_bitmap = 0;
	umem = bnxt_re_dv_umem_reg(ibvctx, &umem_input);
	if (!umem) {
		fprintf(stderr, "%s: RQ umem_reg() failed\n", __func__);
		goto fail;
	}
	qp->rq_umem = umem;

	return 0;

fail:
	if (qp->sq_umem) {
		bnxt_re_dv_umem_dereg(qp->sq_umem);
		qp->sq_umem = NULL;
	}
	return -EIO;
}

static struct ibv_qp *
bnxt_re_dv_create_qp_int(struct ibv_pd *ibvpd,
			 struct bnxt_re_dv_qp_init_attr *dv_qp_attr)
{
	struct bnxt_re_dv_qp_init_attr_internal dv_qp_attr_int = {};
	struct bnxt_re_dv_create_qp_resp resp = {};
	struct ibv_qp_init_attr attr = {};
	struct ibv_qp_init_attr_ex attr_ex;
	struct bnxt_re_qp *qp;
	int ret;

	memset(&attr_ex, 0, sizeof(attr_ex));
	fill_ib_attr_from_dv_qp_attr(dv_qp_attr, &attr);
	memcpy(&attr_ex, &attr, sizeof(attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;
	qp = bnxt_re_dv_alloc_qp(ibvpd->context, &attr_ex);
	if (!qp) {
		fprintf(stderr, "%s: failed\n", __func__);
		return NULL;
	}

	bnxt_re_get_dv_qp_attr(qp, BNXT_RE_QATTR_SQ_INDX, &dv_qp_attr_int);
	bnxt_re_get_dv_qp_attr(qp, BNXT_RE_QATTR_RQ_INDX, &dv_qp_attr_int);
	dv_qp_attr_int.pdid = qp->re_pd->pdid;
	dv_qp_attr_int.qp_handle = (uint64_t)qp;

	bnxt_re_print_dv_qp_attr(&attr_ex, &dv_qp_attr_int);

	ret = bnxt_re_dv_qp_umem_reg(ibvpd->context, qp, &dv_qp_attr_int);
	if (ret) {
		fprintf(stderr,
			"%s: bnxt_re_dv_qp_umem_reg() failed: %d\n",
			__func__, ret);
		bnxt_re_free_mem(qp->mem, NULL);
		return NULL;
	}

	ret = bnxt_re_dv_create_qp_cmd_int(ibvpd->context, &dv_qp_attr_int,
					   dv_qp_attr, &resp, qp);
	if (ret) {
		bnxt_re_free_mem(qp->mem, NULL);
		return NULL;
	}

	bnxt_re_dv_init_qp(ibvpd->context, &attr_ex, qp, &resp);
	return qp->ibvqp;
}

static void bnxt_re_dv_print_qp_mem_info(struct bnxt_re_dv_qp_mem_info *info)
{
	if (!(bnxt_debug_mask & BNXT_DUMP_DV))
		return;

	fprintf(stderr, "\t SQ Info:\n");
	fprintf(stderr, "\t\t va: 0x%" PRIx64 " len: 0x%x slots: 0x%x wqe_sz: 0x%x\n",
		info->sq_va, info->sq_len, info->sq_slots, info->sq_wqe_sz);
	fprintf(stderr, "\t\t psn_sz: 0x%x npsn: 0x%x\n",
		info->sq_psn_sz, info->sq_npsn);

	fprintf(stderr, "\t RQ Info:\n");
	fprintf(stderr, "\t\t va: 0x%" PRIx64 " len: 0x%x slots: 0x%x wqe_sz: 0x%x\n",
		info->rq_va, info->rq_len, info->rq_slots, info->rq_wqe_sz);
}

static void bnxt_re_dv_init_qp_mem_info(struct bnxt_re_dv_qp_mem_info *dst,
					struct bnxt_re_qattr *qattr)
{
	struct bnxt_re_qattr *src;

	src = &qattr[BNXT_RE_QATTR_SQ_INDX];
	dst->sq_va = 0;
	dst->sq_len = src->sz_ring;
	dst->sq_slots = src->slots;
	dst->sq_wqe_sz = src->esize;
	dst->sq_psn_sz = src->psn_sz;
	dst->sq_npsn = src->npsn;

	src = &qattr[BNXT_RE_QATTR_RQ_INDX];
	dst->rq_va = 0;
	dst->rq_len = src->sz_ring;
	dst->rq_slots = src->slots;
	dst->rq_wqe_sz = src->esize;
}

/* Apps can use the helper function below to get the size
 * and other related parameters needed to allocate QP memory,
 * followed by the call to DV_CREATE_QP.
 * This can be used by apps that do not rely on the library
 * for datapath; i.e the app allocates and manages QP memory.
 */
int bnxt_re_dv_qp_get_mem_info(struct ibv_pd *ibvpd,
			       struct ibv_qp_init_attr *attr,
			       struct bnxt_re_dv_qp_mem_info *qp_mem)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvpd->context);
	struct ibv_qp_init_attr_ex attr_ex;
	struct bnxt_re_qattr qattr[2];
	int rc;

	memset(&attr_ex, 0, sizeof(attr_ex));
	memcpy(&attr_ex, attr, sizeof(*attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;

	rc = bnxt_re_check_qp_limits(cntx, &attr_ex);
	if (rc < 0)
		return rc;

	memset(qattr, 0, (2 * sizeof(*qattr)));
	rc = bnxt_re_get_sqmem_size(cntx, &attr_ex, &qattr[BNXT_RE_QATTR_SQ_INDX]);
	if (rc < 0)
		return rc;

	rc = bnxt_re_get_rqmem_size(cntx, &attr_ex, &qattr[BNXT_RE_QATTR_RQ_INDX]);
	if (rc < 0)
		return rc;

	bnxt_re_dv_init_qp_mem_info(qp_mem, qattr);
	bnxt_re_dv_print_qp_mem_info(qp_mem);
	return rc;
}

void bnxt_re_dv_copy_qp_mem_info(struct bnxt_re_dv_qp_mem_info *dst,
				 struct bnxt_re_dv_qp_init_attr_internal *src)
{
	dst->qp_handle = src->qp_handle;
	dst->sq_va = src->sq_va;
	dst->sq_len = src->sq_len;
	dst->sq_slots = src->sq_slots;
	dst->sq_wqe_sz = src->sq_wqe_sz;
	dst->sq_psn_sz = src->sq_psn_sz;
	dst->sq_npsn = src->sq_npsn;
	dst->rq_va = src->rq_va;
	dst->rq_len = src->rq_len;
	dst->rq_slots = src->rq_slots;
	dst->rq_wqe_sz = src->rq_wqe_sz;
	dst->comp_mask = src->comp_mask;
}

/* Overwrite the VA in SQ and RQ to the VA allocated by the app.
 * This is a workaround to validate app allocated memory, while
 * still utilizing the metadata (QP structures) allocated by the
 * library. This is needed since the library allocates both the
 * metadata and the actual queue data memory as one single/large
 * object. And these metadata objects are needed if the datapath
 * provided by the library is used by the app (hybrid mode).
 * This also implies some wastage of queue data memory, but it is
 * ok since this mode would be used only for initial validation
 * of DV verbs.
 */
static void bnxt_re_dv_qp_update_va(struct bnxt_re_qp *qp,
				    struct bnxt_re_dv_umem *sq_umem,
				    struct bnxt_re_dv_umem *rq_umem)
{
	struct bnxt_re_joint_queue *jqq;
	struct bnxt_re_queue *que;
	void *new_va;
	void *pad;

	if (sq_umem) {
		jqq = qp->jsqq;
		que = jqq->hwque;

		new_va = (sq_umem->addr + (qp->qattr[0].sz_ring * sq_umem->umem_qp_count));
		pad = (new_va + que->depth * que->stride);
		bnxt_trace_dv(NULL, "%s: Updating SQ VA from: 0x%lx to: 0x%lx msnp: 0x%lx\n",
			      __func__, (uint64_t)que->va, (uint64_t)new_va, pad);
		que->va = new_va;
		que->pad = pad;
	}
	if (rq_umem) {
		jqq = qp->jrqq;
		que = jqq->hwque;

		new_va = (rq_umem->addr + (qp->qattr[1].sz_ring * rq_umem->umem_qp_count));
		bnxt_trace_dv(NULL, "%s: Updating RQ VA from: 0x%lx to: 0x%lx\n",
			      __func__, (uint64_t)que->va, (uint64_t)new_va);
		que->va = new_va;
	}
}

/* Apps can use the helper function below to allocate
 * QP memory. The flag below to indicate this mode
 * is set if this helper is used. Otherwise, it is
 * assumed that the app allocates and manages QP memory.
 * Also see bnxt_re_dv_create_qp_ext().
 *
 * Note that if the helper is used, the library can
 * handle the datapath (post_send, poll) also; otherwise
 * the app needs to have its own datapath functions.
 */
bool bnxt_re_dv_qp_alloc_helper;

int bnxt_re_dv_qp_mem_alloc(struct ibv_pd *ibvpd,
			    struct ibv_qp_init_attr *attr,
			    struct bnxt_re_dv_qp_mem_info *dv_qp_mem)
{
	struct bnxt_re_dv_qp_init_attr_internal dv_qp_attr_int = {};
	struct ibv_qp_init_attr_ex attr_ex;
	struct bnxt_re_qp *qp;

	memset(&attr_ex, 0, sizeof(attr_ex));
	memcpy(&attr_ex, attr, sizeof(*attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;
	qp = bnxt_re_dv_alloc_qp(ibvpd->context, &attr_ex);
	if (!qp) {
		fprintf(stderr, "%s: failed\n", __func__);
		return -ENOMEM;
	}

	bnxt_re_get_dv_qp_attr(qp, BNXT_RE_QATTR_SQ_INDX, &dv_qp_attr_int);
	bnxt_re_get_dv_qp_attr(qp, BNXT_RE_QATTR_RQ_INDX, &dv_qp_attr_int);
	dv_qp_attr_int.pdid = qp->re_pd->pdid;
	dv_qp_attr_int.qp_handle = (uint64_t)qp;

	bnxt_re_print_dv_qp_attr(&attr_ex, &dv_qp_attr_int);
	bnxt_re_dv_copy_qp_mem_info(dv_qp_mem, &dv_qp_attr_int);
	bnxt_re_dv_qp_alloc_helper = true;

	return 0;
}

static int
bnxt_re_dv_create_qp_cmd_ext(struct ibv_context *ibvctx,
			     struct bnxt_re_dv_qp_init_attr *dv_qp_attr,
			     struct bnxt_re_dv_create_qp_resp *resp,
			     struct bnxt_re_qp *qp)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_QP,
			       BNXT_RE_METHOD_DV_CREATE_QP,
			       9);
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dv_db_region_attr *db_attr = NULL;
	struct bnxt_re_dv_create_qp_req req = {};
	struct bnxt_re_dv_umem *sq_umem = NULL;
	struct bnxt_re_dv_umem *rq_umem = NULL;
	struct ib_uverbs_attr *handle;
	struct bnxt_re_cq *re_cq;
	uint64_t offset;
	uint32_t size;
	int ret;

	req.qp_type = dv_qp_attr->qp_type;
	req.max_send_wr = dv_qp_attr->max_send_wr;
	req.max_recv_wr = dv_qp_attr->max_recv_wr;
	req.max_send_sge = dv_qp_attr->max_send_sge;
	req.max_recv_sge = dv_qp_attr->max_recv_sge;
	req.max_inline_data = dv_qp_attr->max_inline_data;

	req.pd_id = qp->re_pd->pdid;
	req.qp_handle = dv_qp_attr->qp_handle;

	sq_umem = dv_qp_attr->sq_umem_handle;
	fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_SQ_UMEM_HANDLE,
			 sq_umem->handle);

	offset = dv_qp_attr->sq_umem_offset;
	size = dv_qp_attr->sq_len;
	if (!bnxt_re_dv_is_valid_umem(cntx->rdev, sq_umem, offset, size)) {
		fprintf(stderr,
			"%s: Invalid sq_umem: handle: 0x%x offset: 0x%lx size: 0x%x\n",
			__func__, sq_umem->handle, offset, size);
		return -EINVAL;
	}
	bnxt_trace_dv(NULL, "%s: sq_umem: handle: 0x%x offset: 0x%lx size: 0x%x\n",
		      __func__, sq_umem->handle, offset, size);
	req.sq_va = 0;
	req.sq_umem_offset = offset;
	req.sq_len = size;
	req.sq_slots = dv_qp_attr->sq_slots;
	req.sq_wqe_sz = dv_qp_attr->sq_wqe_sz;
	req.sq_psn_sz = dv_qp_attr->sq_psn_sz;
	req.sq_npsn = dv_qp_attr->sq_npsn;

	if (!dv_qp_attr->srq) {
		rq_umem = dv_qp_attr->rq_umem_handle;
		if (rq_umem) {
			fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_RQ_UMEM_HANDLE,
					 rq_umem->handle);
		} else {
			/* App didn't umem register RQ, probably because
			 * app didn't allocate its own RQ memory. We will
			 * register library allocated (internal) memory.
			 */
			struct bnxt_re_dv_umem_reg_attr umem_input;
			struct bnxt_re_dv_umem *umem;

			umem_input.addr = (void *)qp->jrqq->hwque->va;
			umem_input.size = dv_qp_attr->rq_len;
			umem_input.access_flags = IBV_ACCESS_LOCAL_WRITE;
			umem_input.comp_mask = 0;
			umem_input.pgsz_bitmap = 0;
			umem = bnxt_re_dv_umem_reg(ibvctx, &umem_input);
			if (!umem) {
				fprintf(stderr, "%s: RQ umem_reg() failed\n", __func__);
				return -EIO;
			}
			qp->rq_umem = umem;
			fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_RQ_UMEM_HANDLE,
					 qp->rq_umem->handle);
		}

		offset = dv_qp_attr->rq_umem_offset;
		size = dv_qp_attr->rq_len;
		if (!bnxt_re_dv_is_valid_umem(cntx->rdev, rq_umem, offset, size)) {
			fprintf(stderr,
				"%s: Invalid rq_umem: handle: 0x%x offset: 0x%lx size: 0x%x\n",
				__func__, rq_umem->handle, offset, size);
			return -EINVAL;
		}
		bnxt_trace_dv(NULL, "%s: rq_umem: handle: 0x%x offset: 0x%lx size: 0x%x\n",
			      __func__, rq_umem->handle, offset, size);

		req.rq_umem_offset = offset;
		req.rq_len = size;
		req.rq_slots = dv_qp_attr->rq_slots;
		req.rq_wqe_sz = dv_qp_attr->rq_wqe_sz;
	} else {
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_SRQ_HANDLE,
				 dv_qp_attr->srq->handle);
	}

	if (bnxt_re_dv_qp_alloc_helper)
		bnxt_re_dv_qp_update_va(qp, sq_umem, rq_umem);

	fill_attr_in_ptr(cmd, BNXT_RE_DV_CREATE_QP_REQ, &req);

	re_cq = to_bnxt_re_cq(dv_qp_attr->send_cq);
	fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_SEND_CQ_HANDLE,
			 re_cq->verbs_cq.cq.handle);

	re_cq = to_bnxt_re_cq(dv_qp_attr->recv_cq);
	fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_RECV_CQ_HANDLE,
			 re_cq->verbs_cq.cq.handle);

	if (dv_qp_attr->dbr_handle) {
		db_attr = dv_qp_attr->dbr_handle;
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_QP_DBR_HANDLE,
				 db_attr->handle);
		qp->dv_dpi.dbpage = db_attr->dbr;
		qp->dv_dpi.dpindx = db_attr->dpi;
		qp->udpi = &qp->dv_dpi;
	} else {
		qp->udpi = &cntx->udpi;
	}

	/* Output args */
	handle = fill_attr_out_obj(cmd, BNXT_RE_DV_CREATE_QP_HANDLE);
	fill_attr_out_ptr(cmd, BNXT_RE_DV_CREATE_QP_RESP, resp);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		return ret;
	}

	qp->qp_handle = read_attr_obj(BNXT_RE_DV_CREATE_QP_HANDLE, handle);
	if (bnxt_re_dv_qp_alloc_helper) {
		if (sq_umem)
			sq_umem->umem_qp_count++;
		if (rq_umem)
			rq_umem->umem_qp_count++;
	}
	bnxt_trace_dv(NULL, "%s: QP handle: 0x%x qpid: 0x%x\n",
		      __func__, qp->qp_handle, resp->qpid);

	return 0;
}

static struct ibv_qp *
bnxt_re_dv_create_qp_ext(struct ibv_pd *ibvpd,
			 struct bnxt_re_dv_qp_init_attr *dv_qp_attr)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvpd->context);
	struct bnxt_re_dv_create_qp_resp resp = {};
	struct ibv_qp_init_attr_ex attr_ex;
	struct ibv_qp_init_attr attr = {};
	struct bnxt_re_qp *qp;
	int rc;

	if (!bnxt_re_dv_qp_alloc_helper) {
		qp = malloc(sizeof(*qp));
		if (!qp)
			return NULL;

		memset(qp, 0, sizeof(*qp));
		qp->ibvqp = &qp->vqp.qp;
		qp->mem = NULL;
		qp->cctx = cntx->cctx;
		qp->cntx = cntx;
		qp->qpmode = cntx->modes & BNXT_RE_WQE_MODE_VARIABLE;
		qp->re_pd = to_bnxt_re_pd(ibvpd);

		dv_qp_attr->qp_handle = (uint64_t)qp;
	} else {
		qp = (struct bnxt_re_qp *)dv_qp_attr->qp_handle;
		if (!qp)
			return NULL;
	}

	rc = bnxt_re_dv_create_qp_cmd_ext(ibvpd->context, dv_qp_attr, &resp, qp);
	if (rc) {
		if (!bnxt_re_dv_qp_alloc_helper)
			free(qp);
		return NULL;
	}

	memset(&attr_ex, 0, sizeof(attr_ex));
	fill_ib_attr_from_dv_qp_attr(dv_qp_attr, &attr);
	memcpy(&attr_ex, &attr, sizeof(attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;

	bnxt_re_dv_init_qp(ibvpd->context, &attr_ex, qp, &resp);
	return qp->ibvqp;
}

struct ibv_qp *bnxt_re_dv_create_qp(struct ibv_pd *ibvpd,
				    struct bnxt_re_dv_qp_init_attr *dv_qp_attr)
{
	if (!dv_qp_attr->sq_umem_handle)
		return bnxt_re_dv_create_qp_int(ibvpd, dv_qp_attr);
	return bnxt_re_dv_create_qp_ext(ibvpd, dv_qp_attr);
}

int bnxt_re_dv_destroy_qp(struct ibv_qp *ibvqp)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_QP,
			       BNXT_RE_METHOD_DV_DESTROY_QP,
			       1);
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	struct ibv_context *ibvctx;
	struct bnxt_re_mem *mem;
	int ret;

	qp->qpst = IBV_QPS_RESET;
	fill_attr_in_obj(cmd, BNXT_RE_DV_DESTROY_QP_HANDLE, qp->qp_handle);
	bnxt_trace_dv(NULL, "%s: QP handle: 0x%x\n", __func__, qp->qp_handle);

	ibvctx = bnxt_re_to_ibvctx(qp->cntx);
	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		return ret;
	}
	bnxt_re_cleanup_cq(qp, qp->rcq);
	if (qp->scq != qp->rcq)
		bnxt_re_cleanup_cq(qp, qp->scq);
	if (qp->sq_umem) {
		bnxt_re_dv_umem_dereg(qp->sq_umem);
		qp->sq_umem = NULL;
	}
	if (qp->rq_umem) {
		bnxt_re_dv_umem_dereg(qp->rq_umem);
		qp->rq_umem = NULL;
	}
	mem = qp->mem;
	bnxt_re_free_mem(mem, NULL);
	return 0;
}

int bnxt_re_dv_query_qp(void *qp_handle, struct ib_uverbs_qp_attr *qp_attr)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_QP,
			       BNXT_RE_METHOD_DV_QUERY_QP,
			       2);
	struct ibv_qp *ibvqp = qp_handle;
	struct bnxt_re_qp *qp = to_bnxt_re_qp(ibvqp);
	int ret;

	bnxt_trace_dv(NULL, DEV "DV Query QP: handle: 0x%x\n", qp->qp_handle);
	fill_attr_in_obj(cmd, BNXT_RE_DV_QUERY_QP_HANDLE, qp->qp_handle);
	fill_attr_out_ptr(cmd, BNXT_RE_DV_QUERY_QP_ATTR, qp_attr);

	ret = execute_ioctl(qp->ibvqp->context, cmd);
	if (ret)
		fprintf(stderr, DEV "DV Query QP error %d\n", ret);

	return ret;
}

int bnxt_re_dv_destroy_obj(struct ibv_context *ibvctx, uint32_t handle)
{
	int ret = 0;
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_CREATE_DESTROY_OBJ,
			       BNXT_RE_METHOD_DV_DESTROY_OBJ,
			       1);

	fill_attr_in_obj(cmd, BNXT_RE_DV_DESTROY_OBJ_HANDLE, handle);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d, error num = %s\n",
			__func__, ret, strerror(errno));
		return 0;
	}
	return 0;
}

/* Fill umem attr.
 * Returns 0 on success, -EINVAL on validation failure
 */
static int bnxt_re_dv_create_obj_fill_umem_attrs(struct bnxt_re_dev *dev,
						 struct bnxt_re_dv_create_obj_init_attr *attr,
						 void *cmd)
{
	struct bnxt_re_dv_umem *umem;

	umem = (struct bnxt_re_dv_umem *)attr->umem;
	if (!umem) {
		fprintf(stderr, "%s: Invalid umem handle\n", __func__);
		errno = EINVAL;
		return -EINVAL;
	}
	if (!bnxt_re_dv_is_valid_umem(dev, umem, attr->offset, attr->size)) {
		fprintf(stderr, "%s: Invalid handle: 0x%x offset: 0x%lx size: 0x%x\n",
			__func__, umem->handle, attr->offset, attr->size);
		errno = EINVAL;
		return -EINVAL;
	}
	fill_attr_in_obj(cmd, BNXT_RE_CREATE_OBJ_UMEM_HANDLE, umem->handle);

	return 0;
}

/* Fills stats/rx/cq handles for ring object types.
 * Returns 0 on success, -EINVAL on validation failure
 */
static int
bnxt_re_dv_create_obj_fill_ring_attrs(struct bnxt_re_dv_create_obj_init_attr *attr, void *cmd)
{
	struct bnxt_re_cq *re_cq;

	if (!attr->stats_handle) {
		errno = EINVAL;
		return -EINVAL;
	}

	fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_OBJ_STATS_HANDLE, attr->stats_handle);
	if (attr->obj_type == BNXT_RE_OBJ_TYPE_RX_AGGR) {
		if (!attr->rx_handle) {
			errno = EINVAL;
			return -EINVAL;
		}
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_OBJ_RX_HANDLE, attr->rx_handle);
	}

	if (attr->send_cq) {
		re_cq = to_bnxt_re_cq(attr->send_cq);
		fill_attr_in_obj(cmd, BNXT_RE_DV_CREATE_OBJ_CQ_HANDLE,
				 re_cq->verbs_cq.cq.handle);
	}

	return 0;
}

static bool bnxt_re_dv_create_obj_uses_umem(uint8_t obj_type)
{
	return obj_type == BNXT_RE_OBJ_TYPE_STATS ||
	       obj_type == BNXT_RE_OBJ_TYPE_TX ||
	       obj_type == BNXT_RE_OBJ_TYPE_RX ||
	       obj_type == BNXT_RE_OBJ_TYPE_MPC ||
	       obj_type == BNXT_RE_OBJ_TYPE_RX_AGGR;
}

static bool bnxt_re_dv_create_obj_is_ring_type(uint8_t obj_type)
{
	return obj_type == BNXT_RE_OBJ_TYPE_TX ||
	       obj_type == BNXT_RE_OBJ_TYPE_RX ||
	       obj_type == BNXT_RE_OBJ_TYPE_MPC ||
	       obj_type == BNXT_RE_OBJ_TYPE_RX_AGGR;
}

int bnxt_re_dv_create_obj(struct ibv_context *ibvctx,
			  struct bnxt_re_dv_create_obj_init_attr *attr,
			  struct bnxt_re_dv_create_obj_resp *resp)
{
	DECLARE_COMMAND_BUFFER(cmd,
			       BNXT_RE_OBJECT_DV_CREATE_DESTROY_OBJ,
			       BNXT_RE_METHOD_DV_CREATE_OBJ,
			       7);

	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvctx->device);
	struct ib_uverbs_attr *handle;
	int ret;

	if (bnxt_re_dv_create_obj_uses_umem(attr->obj_type)) {
		ret = bnxt_re_dv_create_obj_fill_umem_attrs(dev, attr, cmd);
		if (ret)
			return ret;
	}

	if (bnxt_re_dv_create_obj_is_ring_type(attr->obj_type)) {
		ret = bnxt_re_dv_create_obj_fill_ring_attrs(attr, cmd);
		if (ret)
			return ret;
	} else if (attr->obj_type != BNXT_RE_OBJ_TYPE_STATS &&
		   attr->obj_type != BNXT_RE_OBJ_TYPE_VNIC &&
		   attr->obj_type != BNXT_RE_OBJ_TYPE_BFID &&
		   attr->obj_type != BNXT_RE_OBJ_TYPE_RSS) {
		fprintf(stderr, "%s: Invalid obj_type: %d\n", __func__, attr->obj_type);
		errno = EINVAL;
		return -EINVAL;
	}

	fill_attr_in_ptr(cmd, BNXT_RE_CREATE_OBJ_ATTR_PTR, attr);
	handle = fill_attr_out_obj(cmd, BNXT_RE_CREATE_OBJ_HANDLE);
	fill_attr_out_ptr(cmd, BNXT_RE_CREATE_OBJ_RESP, resp);

	ret = execute_ioctl(ibvctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n", __func__, ret);
		errno = ret;
		return ret;
	}

	if (handle)
		attr->handle = read_attr_obj(BNXT_RE_CREATE_OBJ_HANDLE, handle);
	return 0;
}

int bnxt_re_dv_get_cq_attr(struct ibv_context *ibvctx, uint32_t ncqe,
			   struct bnxt_re_dv_cq_attr *cq_attr)
{
	struct bnxt_re_dev *dev = to_bnxt_re_dev(ibvctx->device);

	if (ncqe > dev->max_cq_depth)
		return -EINVAL;

	if (ncqe * 2 < dev->max_cq_depth)
		ncqe = 2 * ncqe;

	cq_attr->ncqe = ncqe + 1;
	cq_attr->cqe_size = bnxt_re_get_cqe_sz();
	return 0;
}

int bnxt_re_dv_send_pt(struct ibv_context *ctx, struct bnxt_re_dv_pt_msg_input *in)
{
	int ret;

	DECLARE_COMMAND_BUFFER(cmd, BNXT_RE_OBJECT_DV_SEND_PT_MSG,
			       BNXT_RE_METHOD_DV_SEND_PT_MSG, 1);

	fill_attr_in(cmd, BNXT_RE_SEND_PT_MSG_IN, in, sizeof(*in));

	ret = execute_ioctl(ctx, cmd);
	if (ret) {
		fprintf(stderr, "%s: execute_ioctl() failed: %d\n",
			__func__, ret);
		errno = ret;
	}
	return ret;
}
