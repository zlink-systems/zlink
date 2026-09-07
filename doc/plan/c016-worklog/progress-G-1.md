# G-1 진행

- 00:00 브리프·공통규칙·G-A·S-1/S-2/S-11/review-S-2 읽음. worktree ~/project/zlink-work/g1 (2d56078977) 생성.
- G-A callgrind 덤프(cg_st_zlink_full.out, 15377 msg)를 caller별로 재파싱해 msg당 lock 표 확보(아래).
  mutex_t::lock 9.068 + process_commands 직접 2.009 + asio_poller_t::loop 1.586 + asio 내부 ~2.66 + std::mutex 0.492 ≈ 17.33
- 다음: pipe_t::write / write_single_message_and_flush (같은 _out_sync 2회/msg) 소스 확인
- 00:25 release LTO 덤프는 라인정보가 없어 process_commands 내부 잠금(2.009/msg)을 구분 못함.
  dev(RelWithDebInfo, LTO OFF, -g) lib로 callgrind를 다시 떠서 caller×line 분해하기로. GA 워크트리의 with_stream 바이너리를 LD_LIBRARY_PATH로 재사용.
- 00:26 dev 빌드 시작 (JOBS=4)
- 01:00 dev callgrind before = 17.396 lock/msg. caller×line 표 확보(mailbox_t::send 2.013, process_commands 1.558,
  asio_poller_t::loop 1.155, process_deferred_socket_msg_pipe_terminations 1.004, pipe_t::flush 1.002,
  pipe_t::write 1.000, write_single_message_and_flush 1.000, stream_t::xsend_routed 1.000, read_activated 0.999,
  refresh_application_hwm_if_drained 0.499, has_in 0.340, mailbox_t::recv 0.340, ...)
- 01:02 변경 2건 적용: (1) deferred termination 큐 head를 atomic으로 두고 빈 큐 프로브를 잠금 밖으로,
  (2) refresh_application_hwm_if_drained의 planned==applied 조기반환을 ctx 전역 _sync 밖으로. 빌드 시작.
- 01:40 after 측정 15.122 lock/msg(직접 귀속 −1.503), ctest 8회(7회 clean, 1회 미규명 실패), 보고서 작성 완료. 상한 도달로 TSan·with_stream 미실행.
