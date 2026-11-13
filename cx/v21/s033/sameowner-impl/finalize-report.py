from pathlib import Path
import subprocess, json, re, os
job=Path(__file__).parent
root=Path('/root/worktree/zlink-v21-s033/sameowner-impl')
product='985c271c1b998507c4956965dec16008e2c0764a'
base='3ea1463fd2c4e1e75bb4d593d0f91fdab697213b'
subprocess.run(['python3',str(job/'summarize-final.py')],check=True)
assert (job/'repeat-counts.log').read_text().count('pass=50/50; lost/duplicate/wrapper detected=[50, 50, 50]')==16
results=json.loads((job/'repeat-final-summary.json').read_text())
assert len(results)==101 and all(r['exit']==0 for r in results)
for name in ('repeat-final','fixtures-targeted','owner-dispatch-repeat','build-fixtures','format-check-fixtures-final','build-transfer','format-check-transfer'):
    assert (job/(name+'.exit')).read_text().strip()=='0', name
suite=(job/'full-suite.log').read_text()
assert re.search(r'98% tests passed, 6 tests failed out of 304',suite)
failed=[line.split(':',1)[1] for line in (job/'full-suite-LastTestsFailed.log').read_text().splitlines()]
target=(job/'fixtures-targeted.log').read_text()
assert '100% tests passed, 0 tests failed out of 1' in target
prior=(job/'fixtures-targeted-attempt1.log').read_text()
lines=[]
for name in failed:
    evidence=target if name=='test_cpp_framework_m6b_runtime' else prior
    assert re.search(re.escape(name)+r'\s+\.+\s+Passed',evidence),name
    origin='fixtures-targeted.log' if name=='test_cpp_framework_m6b_runtime' else 'fixtures-targeted-attempt1.log'
    lines.append(name+': pass=1/1; evidence='+origin)
(job/'fixture-results.log').write_text('\n'.join(lines)+'\n')
legacy=json.loads((job/'owner-dispatch-repeat-summary.json').read_text())
assert len(legacy)==50 and all(r['exit']==0 for r in legacy)
negative=[sum(Path(r['log']).read_text().count('negative control fault='+str(i)+' detected=1') for r in legacy) for i in (1,2,3)]
assert negative==[50,50,50]
(job/'owner-dispatch-counts.log').write_text('pass=50/50 lost/duplicate/wrapper detected='+str(negative)+'\n')
status=subprocess.check_output(['git','status','--porcelain=v1'],cwd=root)
assert not status
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
branch=subprocess.check_output(['git','branch','--show-current'],cwd=root,text=True).strip()
assert branch=='perf/1639-cpp-same-owner'
paths=(job/'product-files.txt').read_text().splitlines()
assert subprocess.run(['git','diff','--quiet',product,'HEAD','--',*paths],cwd=root).returncode==0
focused=['test_cpp_framework_receive_role_handoff.cpp','test_cpp_framework_client_server_runtime.cpp','same_owner_claim_controls.hpp']
focused=['framework/languages/cpp/tests/Zlink.Framework.UnitTests/'+p for p in focused]
assert subprocess.run(['git','diff','--quiet',product,'HEAD','--',*focused],cwd=root).returncode==0
(job/'final-repository.log').write_text(f'commit={sha}\nbranch={branch}\nstatus_bytes={len(status)}\nproduct_source_diff_exit=0\nfocused_test_source_diff_exit=0\n')
with (job/'final-binary-verify.log').open('w') as log:
    subprocess.run(['sha256sum','-c',str(job/'final-binary-sha256.log')],cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
changed=subprocess.check_output(['git','diff','--name-only',base,'HEAD'],cwd=root,text=True).splitlines()
assert len(changed)==28
(job/'changed-files.log').write_text('\n'.join(changed)+'\n')
(job/'changed-files-count.log').write_text('total=28, product=7, tests=20, cmake=1\n')
(job/'full-suite-counts.log').write_text('initial_total=304 initial_passed=298 initial_failed=6\nretested=6 passed=6 failed=0; first_subset=5/6 last_m6b=1/1\nremaining_failures=0; full_suite_invocations=1\n')
size=int(subprocess.check_output(['du','-sb',str(root/'build-sameowner')],text=True).split()[0])
assert size<25000000000
(job/'build-size-final.log').write_text(f'{size}\t{root/"build-sameowner"}\n')
report=(job/'report.md').read_text().replace('— 최종 검증 대기','— 구현·검증 완료',1)
a=report.index('제품 commit '); b=report.index('\n\n경로:',a)
intro=f'제품 commit `985c271c1b`, 시험 보완 commit `e7fe9fee89`·`{sha[:10]}`로 완료했다. 제품 7개, 시험 20개, CMake 1개 파일을 변경했다(`commit.log:1`, `final-repository.log:1`, `changed-files-count.log:1`; 목록 `changed-files.log:1`). 선택 시험 16개는 각각 **50/50 통과**, 세 실패 주입도 각각 **50/50 검출**했다(`repeat-counts.log:1`). 전체 1회에서 298/304 통과·6개 실패를 확인했고, 관련 fixture·라벨 수정 후 **관련 6개 모두 통과**했다(1차 5개·2차 m6b 1개). 남은 실패는 0건이다(`full-suite-counts.log:1`, `fixture-results.log:1`).'
report=report[:a]+intro+report[b:]
a=report.index('## 빌드와 남은 검증')
report=report[:a]+f'''## 빌드·재검증과 한계

- 보존한 build: `/root/worktree/zlink-v21-s033/sameowner-impl/build-sameowner`, {size:,} bytes (`build-size-final.log:1`). `linux-ninja-release`, Framework tests ON, binding source ON, Core release `/root/.cache/zlink/core-release/1.24.0/linux-x64` 구성이다(`build-config-final.log:1`, `:6`, `:9`, `ldd-fail-before.log:2`).
- `cpp-build-slot.sh`, `-j3 -k0`, 주소공간 상한 26,843,545,600 bytes로 전체 빌드·최종 재링크·시험 보완 재빌드를 완료했다(`build-resource-limits.log:3`, `build-product-all.log:593`, `build-final.log:136`, `build-fixtures.exit:1`, `build-transfer.exit:1`). cpp format도 통과했다(`format-check-transfer.exit:1`).
- 기존 fixture는 한 번의 Node dispatch에 batch 전체를 기대하거나 handler 진입을 직렬로 가정했다. 3개 record FIFO는 claim 3회로, 64개 오류 회계는 record별 실패 64회로 검사해 기존 총량 검사를 유지했다(`T/test_cpp_framework_m6b_runtime.cpp:4113`, `T/test_cpp_framework_maintenance_failure.cpp:114`, `T/test_cpp_framework_relocation_seal_contract.cpp:1732`).
- m6b의 ACK marker·multicast 검사는 기존 raw consumer fixture로 원본을 mutex 안에서 옮겨 payload·routing·metadata 검사를 유지한다(`T/test_cpp_framework_m6b_runtime.cpp:4416`, `:4570`).
- Node bridge는 세 handler가 완료 0건인 동안 진입하고, 반환한 capacity를 기존 FIFO permit API로 확인한다(`T/test_cpp_framework_mesh_node_vertical.cpp:1446`, `:1485`). transport 시험은 A 해제 전 128개 각각 한 번 처리를 확인하며, coalescing은 공통 실패 주입 관측기로 검사한다(`T/test_cpp_framework_mailbox_owner_dispatch.cpp:96`, `:160`, `:292`). 이 transport 시험도 50/50 통과했고 세 변형을 각 50회 검출했다(`owner-dispatch-counts.log:1`).
- 새 CTest는 기존 등록 helper가 contract tier 하나를 부여한다. 잘못된 두 tier는 최초 label-contract 검사에서 검출했고, 수정 후 검사는 통과했다(`full-suite.log:257`, `fixture-results.log:3`, `framework/languages/cpp/CMakeLists.txt:950`).
- 시험 보완에서 제품 파일과 선택 시험 본문은 변경하지 않았다. 선택 시험 바이너리·제품 archive hash도 최초 50회 실행과 동일하다(`final-repository.log:4`, `:5`, `final-binary-verify.log:1`). 전체 suite는 요청대로 1회만 실행했고 보완한 6개를 다시 검사했다(`full-suite-counts.log:1`).
- 알려진 실패 0건이다. 검증 범위는 Linux Release/C++이며 다른 OS·교차언어 실행과 성능 측정은 포함하지 않는다(`full-suite-counts.log:3`, `validate-fixtures.py:1`, `../sameowner2/report.md:55`).

리팩토링 점검: 성능 1건, POSDDD 1건, 불필요 코드 6건 발견. 독립 owner의 불필요한 직렬 실행, 중복 판정·key 해석, 읽지 않는 counter 5개와 기존 CV wake 시험 관측기를 정리했다. 시험의 record별 중복 검사 배열을 추가하면서 관측기의 벡터·counter·flag·helper·wrap을 제거했다. 넘긴 항목 0건이다(`M:139`, `M:145`, `decision-counter-audit.log:1`, `T/test_cpp_framework_mailbox_owner_dispatch.cpp:77`, `fixture-corrections.diff:1`).
'''
tmp=job/'report-final.tmp'; tmp.write_text(report); os.replace(tmp,job/'report.md')
print('Final report complete: '+str(job/'report.md'),flush=True)
