START detached@5e26e7280698; scope=core/src/**,core/tests/**; preserved=core/build-main-readonly; principles reviewed
BASELINE hotpath_gate PASS dd=3437.307050 dr=12015.504600 pair=2681.633700 rr=2967.695500
EDIT removed unused pending-target hooks/dummy command/test; centralized DONTWAIT writable fallback; renamed blocking-wait ownership and socket_send_submit module
VERIFY dev build PASS; changed suites phase3_completion/request_writable/flow_state_c_api/stream_send_blocking_wakeup/socket_runtime PASS 5x each
VERIFY final full ctest excluding hotpath_gate PASS 141/141 in 185.37s; diff checks clean; public header mirrors PASS 8x4
HOTPATH post PASS dd=3438.160250(+0.0248%) dr=12013.766600(-0.0145%) pair=2682.652000(+0.0380%) rr=2968.336000(+0.0216%)
AUDIT target residue rg=0; public headers/docs/bindings unchanged; 19 integration tests with internal includes reported without edits
EXIT:0
