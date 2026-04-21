rdma_shared_provider(bnxt_re libbnxt_re.map
  1 1.0.${PACKAGE_VERSION}
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

rdma_pkg_config("bnxt_re" "libibverbs" "${CMAKE_THREAD_LIBS_INIT}")
