# S-3 진행 (decoder 버퍼 재사용 확대)

- [x] 브리프·S-A §0/§3/§4#5·S-B 3b#3 정독
- [x] worktree ~/project/zlink-work/s3 (detached 430abce139)
- [x] 현행 구조 파악: spare 1칸 CAS, raw_decoder max_messages_=1, allocate() 2회/msg
- [ ] 설계 A(다중 spare 슬롯) 구현
- [ ] dev 빌드 + ctest 5회
- [ ] 축소 callgrind 셀 (after)
- [ ] with_stream 성능 셀
- [ ] ASan 1회
- [ ] 보고서
