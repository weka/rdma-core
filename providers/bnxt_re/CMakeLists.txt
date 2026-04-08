rdma_shared_provider(bnxt_re libbnxt_re.map
  1 1.0.0
  bnxt_re_dv.c
  db.c
  main.c
  memory.c
  verbs.c
)

publish_headers(infiniband
  bnxt_re_dv.h
)

target_link_libraries(bnxt_re LINK_PRIVATE m)
if (ENABLE_STATIC)
  target_link_libraries(bnxt_re-static LINK_PRIVATE m)
endif()
