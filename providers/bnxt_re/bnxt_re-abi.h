/*
 * Broadcom NetXtreme-E User Space RoCE driver
 *
 * Copyright (c) 2015-2025, Broadcom. All rights reserved.
 *
 * Compatibility header: bridges the 238 abi.h with v47 DECLARE_DRV_CMD macros.
 */

#ifndef __BNXT_RE_DRV_ABI_H__
#define __BNXT_RE_DRV_ABI_H__

/*
 * Include the 238 ABI definitions (wire protocol structs, etc.)
 * This replaces the old split between bnxt_re-abi.h + kernel-abi headers.
 */
#include "abi.h"

#include <infiniband/kern-abi.h>

/*
 * DRV_CMD wrappers used by dv.c and verbs.c.
 * These use bnxt_re_cq_req / bnxt_re_cq_resp from abi.h or kernel headers.
 * We provide minimal struct wrappers that match what dv.c expects.
 */

/* The kernel-side CQ req/resp used by DECLARE_DRV_CMD */
#ifndef __BNXT_RE_CQ_REQ_DEFINED__
#define __BNXT_RE_CQ_REQ_DEFINED__
struct bnxt_re_cq_req_drvcmd {
	__aligned_u64 cq_va;
	__aligned_u64 cq_handle;
};

struct bnxt_re_cq_resp_drvcmd {
	__u32 cqid;
	__u32 tail;
	__u32 phase;
	__u32 rsvd;
};
#endif

/*
 * ubnxt_re_cq / ubnxt_re_cq_resp: DRV_CMD wrappers for CQ create.
 * Normally created via DECLARE_DRV_CMD macro, but we build them manually
 * to avoid depending on kernel-abi headers.
 */
struct ubnxt_re_cq {
	struct ibv_create_cq ibv_cmd;
	struct bnxt_re_cq_req_drvcmd drv;
	__aligned_u64 cq_va;
	__aligned_u64 cq_handle;
};

struct ubnxt_re_cq_resp {
	struct ib_uverbs_create_cq_resp ibv_resp;
	__u32 cqid;
	__u32 tail;
	__u32 phase;
	__u32 rsvd;
};

/* ABI version */
#ifndef BNXT_RE_ABI_VERSION
#define BNXT_RE_ABI_VERSION 1
#endif

#endif /* __BNXT_RE_DRV_ABI_H__ */
