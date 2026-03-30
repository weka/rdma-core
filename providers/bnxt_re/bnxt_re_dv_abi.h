/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause) */
/*
 * Broadcom NetXtreme-E RoCE driver.
 *
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
 * Description: DV ioctl ABI definitions (extends kernel bnxt_re-abi.h without
 *              redefining types already present in the kernel header or
 *              bnxt_re_dv.h).
 */

#ifndef __BNXT_RE_DV_ABI_H__
#define __BNXT_RE_DV_ABI_H__

#include <linux/types.h>
#include <rdma/ib_user_ioctl_cmds.h>

/*
 * The kernel header <rdma/bnxt_re-abi.h> defines:
 *   enum bnxt_re_shpg_offt          (BNXT_RE_BEG_RESV_OFFT, etc.)
 *   enum bnxt_re_objects             with only BNXT_RE_OBJECT_ALLOC_PAGE
 *   enum bnxt_re_alloc_page_type     with only BNXT_RE_ALLOC_WC_PAGE
 *   enum bnxt_re_var_alloc_page_attrs
 *   enum bnxt_re_alloc_page_attrs
 *   enum bnxt_re_alloc_page_methods
 *
 * We extend those enums here using #define to avoid redefinition conflicts.
 *
 * Note: BNXT_RE_DV_OBJ_*, BNXT_RE_DV_UMEM_FLAGS_*, and BNXT_RE_DV_CQ_FLAG_*
 * are defined in bnxt_re_dv.h and are not repeated here.
 */

/* Additional bnxt_re_objects values beyond BNXT_RE_OBJECT_ALLOC_PAGE */
#define BNXT_RE_OBJECT_NOTIFY_DRV            (BNXT_RE_OBJECT_ALLOC_PAGE + 1)
#define BNXT_RE_OBJECT_GET_TOGGLE_MEM        (BNXT_RE_OBJECT_ALLOC_PAGE + 2)
#define BNXT_RE_OBJECT_DBR                   (BNXT_RE_OBJECT_ALLOC_PAGE + 3)
#define BNXT_RE_OBJECT_UMEM                  (BNXT_RE_OBJECT_ALLOC_PAGE + 4)
#define BNXT_RE_OBJECT_DV_CQ                 (BNXT_RE_OBJECT_ALLOC_PAGE + 5)
#define BNXT_RE_OBJECT_DV_QP                 (BNXT_RE_OBJECT_ALLOC_PAGE + 6)
#define BNXT_RE_OBJECT_DV_SEND_FW_MSG        (BNXT_RE_OBJECT_ALLOC_PAGE + 7)
#define BNXT_RE_OBJECT_DV_CREATE_DESTROY_OBJ (BNXT_RE_OBJECT_ALLOC_PAGE + 8)
#define BNXT_RE_OBJECT_DV_SEND_PT_MSG        (BNXT_RE_OBJECT_ALLOC_PAGE + 9)

/* Additional bnxt_re_alloc_page_type values beyond BNXT_RE_ALLOC_WC_PAGE */
#define BNXT_RE_ALLOC_DBR_BAR_PAGE           1
#define BNXT_RE_ALLOC_DBR_PAGE               2

/* notify_drv methods */
enum bnxt_re_notify_drv_methods {
	BNXT_RE_METHOD_NOTIFY_DRV = (1U << UVERBS_ID_NS_SHIFT),
};

/* Toggle mem */
enum bnxt_re_get_toggle_mem_type {
	BNXT_RE_CQ_TOGGLE_MEM = 0,
	BNXT_RE_SRQ_TOGGLE_MEM,
};

enum bnxt_re_var_toggle_mem_attrs {
	BNXT_RE_TOGGLE_MEM_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_TOGGLE_MEM_TYPE,
	BNXT_RE_TOGGLE_MEM_RES_ID,
	BNXT_RE_TOGGLE_MEM_MMAP_PAGE,
	BNXT_RE_TOGGLE_MEM_MMAP_OFFSET,
	BNXT_RE_TOGGLE_MEM_MMAP_LENGTH,
};

enum bnxt_re_toggle_mem_attrs {
	BNXT_RE_RELEASE_TOGGLE_MEM_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_toggle_mem_methods {
	BNXT_RE_METHOD_GET_TOGGLE_MEM = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_METHOD_RELEASE_TOGGLE_MEM,
};

/* DBR object methods and attrs */
enum bnxt_re_obj_dbr_alloc_attrs {
	BNXT_RE_DV_ALLOC_DBR_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_DV_ALLOC_DBR_ATTR,
	BNXT_RE_DV_ALLOC_DBR_OFFSET,
};

enum bnxt_re_obj_dbr_free_attrs {
	BNXT_RE_DV_FREE_DBR_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_obj_dbr_query_attrs {
	BNXT_RE_DV_QUERY_DBR_ATTR = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_obj_dbr_methods {
	BNXT_RE_METHOD_DBR_ALLOC = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_METHOD_DBR_FREE,
	BNXT_RE_METHOD_DBR_QUERY,
};

/* UMEM object methods and attrs */
enum bnxt_re_dv_umem_reg_attrs {
	BNXT_RE_UMEM_OBJ_REG_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_UMEM_OBJ_REG_ADDR,
	BNXT_RE_UMEM_OBJ_REG_LEN,
	BNXT_RE_UMEM_OBJ_REG_ACCESS,
	BNXT_RE_UMEM_OBJ_REG_DMABUF_FD,
	BNXT_RE_UMEM_OBJ_REG_PGSZ_BITMAP,
	BNXT_RE_UMEM_OBJ_REG_PA_ARR,
	BNXT_RE_UMEM_OBJ_REG_PA_ARR_LEN,
	BNXT_RE_UMEM_OBJ_REG_MAP_PG_SZ,
};

enum bnxt_re_dv_umem_dereg_attrs {
	BNXT_RE_UMEM_OBJ_DEREG_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_dv_umem_methods {
	BNXT_RE_METHOD_UMEM_REG = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_METHOD_UMEM_DEREG,
};

/* DV CQ request/response structures */
struct bnxt_re_dv_cq_req {
	__u32 ncqe;
	__aligned_u64 va;
	__aligned_u64 comp_mask;
};

struct bnxt_re_dv_cq_resp {
	__u32 cqid;
	__u32 tail;
	__u32 phase;
	__u32 rsvd;
	__aligned_u64 comp_mask;
};

/* DV CQ create/destroy attrs and methods */
enum bnxt_re_dv_create_cq_attrs {
	BNXT_RE_DV_CREATE_CQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_DV_CREATE_CQ_REQ,
	BNXT_RE_DV_CREATE_CQ_UMEM_HANDLE,
	BNXT_RE_DV_CREATE_CQ_UMEM_OFFSET,
	BNXT_RE_DV_CREATE_CQ_RESP,
};

enum bnxt_re_dv_destroy_cq_attrs {
	BNXT_RE_DV_DESTROY_CQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_dv_cq_methods {
	BNXT_RE_METHOD_DV_CREATE_CQ = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_METHOD_DV_DESTROY_CQ,
};

/* DV QP request/response structures */
struct bnxt_re_dv_create_qp_req {
	int qp_type;
	__u32 max_send_wr;
	__u32 max_recv_wr;
	__u32 max_send_sge;
	__u32 max_recv_sge;
	__u32 max_inline_data;
	__u32 pd_id;
	__aligned_u64 qp_handle;
	__aligned_u64 sq_va;
	__u32 sq_umem_offset;
	__u32 sq_len;   /* total len including MSN area */
	__u32 sq_slots;
	__u32 sq_wqe_sz;
	__u32 sq_psn_sz;
	__u32 sq_npsn;
	__aligned_u64 rq_va;
	__u32 rq_umem_offset;
	__u32 rq_len;
	__u32 rq_slots; /* == max_recv_wr */
	__u32 rq_wqe_sz;
};

struct bnxt_re_dv_create_qp_resp {
	__u32 qpid;
};

/* DV QP create/destroy/modify/query attrs and methods */
enum bnxt_re_dv_create_qp_attrs {
	BNXT_RE_DV_CREATE_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_DV_CREATE_QP_REQ,
	BNXT_RE_DV_CREATE_QP_SEND_CQ_HANDLE,
	BNXT_RE_DV_CREATE_QP_RECV_CQ_HANDLE,
	BNXT_RE_DV_CREATE_QP_SQ_UMEM_HANDLE,
	BNXT_RE_DV_CREATE_QP_RQ_UMEM_HANDLE,
	BNXT_RE_DV_CREATE_QP_SRQ_HANDLE,
	BNXT_RE_DV_CREATE_QP_DBR_HANDLE,
	BNXT_RE_DV_CREATE_QP_RESP
};

enum bnxt_re_dv_qp_methods {
	BNXT_RE_METHOD_DV_CREATE_QP = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_METHOD_DV_DESTROY_QP,
	BNXT_RE_METHOD_DV_MODIFY_QP,
	BNXT_RE_METHOD_DV_QUERY_QP,
};

enum bnxt_re_dv_destroy_qp_attrs {
	BNXT_RE_DV_DESTROY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_dv_query_qp_attrs {
	BNXT_RE_DV_QUERY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_DV_QUERY_QP_ATTR,
};

enum bnxt_re_dv_modify_qp_type {
	BNXT_RE_DV_MODIFY_QP_TYPE_NONE = 0,
	BNXT_RE_DV_MODIFY_QP_UDP_SPORT = 1,
};

enum bnxt_re_var_dv_modify_qp_attrs {
	BNXT_RE_DV_MODIFY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_DV_MODIFY_QP_TYPE,
	BNXT_RE_DV_MODIFY_QP_VALUE,
	BNXT_RE_DV_MODIFY_QP_REQ,
};

/* DV send_fw_msg methods and attrs */
enum bnxt_re_dv_send_fw_msg_methods {
	BNXT_RE_METHOD_DV_SEND_FW_MSG = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_METHOD_DV_CREATE_OBJ,
	BNXT_RE_METHOD_DV_DESTROY_OBJ
};

enum bnxt_re_dv_create_obj_attrs {
	BNXT_RE_CREATE_OBJ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_CREATE_OBJ_UMEM_HANDLE,
	BNXT_RE_DV_CREATE_OBJ_CQ_HANDLE,
	BNXT_RE_DV_CREATE_OBJ_STATS_HANDLE,
	BNXT_RE_DV_CREATE_OBJ_RX_HANDLE,
	BNXT_RE_CREATE_OBJ_RESP,
	BNXT_RE_DV_DESTROY_OBJ_HANDLE,
	BNXT_RE_CREATE_OBJ_ATTR_PTR,
};

enum bnxt_re_dv_send_fw_msg_attrs {
	BNXT_RE_SEND_FW_MSG_IN_LEN = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_SEND_FW_MSG_IN,
	BNXT_RE_SEND_FW_MSG_OUT_LEN,
	BNXT_RE_SEND_FW_MSG_OUT,
};

/* DV send_pt_msg methods and attrs */
enum bnxt_re_dv_send_pt_msg_methods {
	BNXT_RE_METHOD_DV_SEND_PT_MSG = (1U << UVERBS_ID_NS_SHIFT),
};

enum bnxt_re_dv_send_pt_msg_attrs {
	BNXT_RE_SEND_PT_MSG_IN_LEN = (1U << UVERBS_ID_NS_SHIFT),
	BNXT_RE_SEND_PT_MSG_IN,
};

#endif /* __BNXT_RE_DV_ABI_H__ */
