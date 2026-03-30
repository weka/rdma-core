# Broadcom bnxt_re provider with direct verbs (DV) support
# Replaces stock bnxt_re with Broadcom bifurcated release 238

# Compatibility defines (replacing autotools config.h)
add_definitions(
  -DHAVE_CONFIG_H=0
  -DHAVE_RDMA_CORE_PKG=1
  -DHAVE_IBV_CMD_MODIFY_QP_EX=1
  -DIBVERBS_PABI_VERSION=34
)

rdma_shared_provider(bnxt_re bnxt_re.map
  1 1.0.0
  bnxt_re_dv.c
  db.c
  debug.c
  main.c
  memory.c
  verbs.c
)

publish_headers(infiniband
  bnxt_re_dv.h
)
