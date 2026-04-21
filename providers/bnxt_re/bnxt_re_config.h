/*
 * bnxt_re provider compile-time configuration.
 *
 * Upstream libbnxt_re uses autoconf to probe the installed rdma-core and
 * generate this file. Here we are part of the rdma-core tree, so the answers
 * are fixed: the API surface matches rdma-core v60+ (covers v61, v62).
 *
 * Keep this as a single source of truth so the #ifdef blocks sprinkled through
 * main.c/verbs.c/compat.h/etc. resolve to the "modern rdma-core" branch.
 */

#ifndef __BNXT_RE_CONFIG_H__
#define __BNXT_RE_CONFIG_H__

/* We are rdma-core. */
#define HAVE_RDMA_CORE_PKG 1

/* rdma-core v60+ API contract. */
#define HAVE_IBV_CMD_MODIFY_QP_EX 1
#define IBV_CMD_CREATE_CQ_EX_HAS_9_ARG 1
#define IBV_CMD_CREATE_CQ_EX_HAS_CONST_CQ_INIT_ATTR 1
#define HAVE_IBV_MR_INIT_ATTR 1
#define HAVE_IBV_FD_ARRAY 1
#define IBV_CMD_REG_DMABUF_MR_HAS_8_ARG 1
#define IBV_CMD_CREATE_FLOW_HAS_5_ARG 1
#define VERBS_ONLY_QUERY_DEVICE_EX_DEFINED 1
#define IBV_CMD_MODIFY_QP_EX_HAS_7_ARG 1
#define IBV_FREE_CONTEXT_IN_CONTEXT_OPS 1
#define REG_MR_VERB_HAS_5_ARG 1
#define IBV_CMD_ALLOC_MW_HAS_1_ARG 1
#define PROVIDER_DRIVER_HAS_2_ARGS 1
#define ALLOC_CONTEXT_HAS_PRIVATE_DATA 1
#define VERBS_MR_DEFINED 1
#define VERBS_INIT_AND_ALLOC_CONTEXT_HAS_5_ARG 1
#define RCP_HAS_PROVIDER_DRIVER 1
#define RCP_USE_IB_UVERBS 1
#define RCP_USE_ALLOC_CONTEXT 1
#define HAVE_IBV_DMABUF 1
#define IBV_CMD_CREATE_QP_EX_HAS_7_ARG 1
#define HAVE_IBV_WR_API 1
#define VERBS_CQ_DEFINED 1

/* verbs/uverbs features present in any supported rdma-core. */
#define HAVE_WR_BIND_MW 1
#define HAVE_SEND_WITH_INV 1
#define HAVE_LOCAL_INV 1
#define HAVE_DRIVER1 1
#define HAVE_FLOW_SPEC 1
#define HAVE_RDMA_DRIVER_ID 1
#define HAVE_UVERBS_CQ_MOD_ST 1
#define HAVE_UVERBS_EX_MODIFY_CQ_ST 1
#define HAVE_ECE_OPTIONS 1
#define HAVE_J8916_ENABLED 1
#define SCHED_YIELD_DEFINED 1

/*
 * HAVE_ST64B is ARM64-v9.2+ only; leave undefined so the fallback path is
 * taken on all architectures. Same for the optional debug knobs.
 */

#endif /* __BNXT_RE_CONFIG_H__ */
