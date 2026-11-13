import json
import pathlib
import re

j = pathlib.Path('/root/jobs/fx/cx/v21/s033/cproto')
s = json.loads((j / 'summary.json').read_text())
pattern = 'request-backpressure'
final_side = 'final-shared-writer' if s.get('final-shared-writer', {}).get(pattern) else 'final'
main = s['main'][pattern]['median']['caller_us']
lines = ['| build | caller µs/request | req/s | Δ caller vs main |',
         '|---|---:|---:|---:|']
labels = {'main': 'main 2f8b8e635a', 'step2': '1–2단계',
          'step4': '1–4단계', 'final': '1–5단계 (정적 작성 규칙 통합 전)' if final_side != 'final' else '최종 prototype'}
if final_side != 'final':
    labels[final_side] = '최종 prototype'
for side, label in labels.items():
    p = s[side].get(pattern)
    if p:
        m = p['median']
        lines.append(f"| {label} | {m['caller_us']:.3f} | {m['req_s']:,.3f} | {m['caller_us']-main:+.3f} |")
    else:
        lines.append(f'| {label} | 측정 못 함 | 측정 못 함 | 측정 못 함 |')
test_path = j / 'ctest-final.log'
test_text = test_path.read_text() if test_path.exists() else ''
if not re.search(r'100% tests passed, 0 tests failed out of 297', test_text):
    lines += ['', '전체 unit·contract 검증은 아직 완료하지 못했다. 아래 성능값은 측정 완료·전체 검증 미완료 상태의 prototype 결과다. 계약 보존의 최종 판정은 남아 있다.']
f = s[final_side].get(pattern)
if f:
    value = f['median']['caller_us']
    lines += ['', f'현행 계약 안 도달 caller µs는 이 prototype에서 {value:.3f} µs/request다. 6.6까지 남은 몫은 {value-6.6:.3f} µs/request다. Main 대비 caller 변화는 {value-main:+.3f} µs/request, {(value-main)/main*100:+.2f}%다.']
    rate_change = (f['median']['req_s'] / s['main'][pattern]['median']['req_s'] - 1) * 100
    lines += ['', f'최종 처리율 중앙값의 main 대비 변화는 {rate_change:+.2f}%다. Caller 6.6 µs 목표에는 도달하지 못했다.']
else:
    lines += ['', '현행 계약 안 도달 caller µs와 6.6까지 남은 몫의 최종 prototype 값은 측정 못 함이다.']
lines += ['', '이 값은 이번 구현으로 실제 도달한 측정점이다. Transport 이름 조회와 variant 분기가 남아 있어 요청한 다섯 항목의 전체 삭제를 완료한 실험은 아니다. 현행 계약의 전체 도달 한계로 해석하지 않는다. Framework÷binding ≥0.6과 caller 6.6 µs 목표는 유지한다. Binding을 이번에 재측정하지 않았으므로 비율 목표 달성을 주장하지 않는다.',
          '', '잔존 위치는 channel_runtime.cpp:1754의 이름 map 조회와 1761–1763의 variant 접근이다. channel.hpp:914의 get<TRequest>()도 기존 bounded cache 경로로 남았다. Bench client는 bench_cpp_client.cpp:883에서 매 요청 call을 새로 만들므로 조회를 call 생성으로 옮기기만 하면 같은 submit thread의 요청당 CPU가 남는다. 추가 cache·flag 없이 bind 결과를 caller가 직접 재사용하도록 연결하는 부분은 이 prototype에서 구현하지 못했다. 이 항목의 완전 삭제 효과는 측정 못 함이다.',
          '', '6.6까지 남은 작업에는 이번에 끝내지 못한 transport 조회·variant 분기와 계약상 유지한 typed encode, 동적 header 기록, service framing, deadline·flow 캡처, C1/C2 등록, 최종 receiver 연결, 실제 binding admission, application 관찰 비용이 포함된다. 이 항목들의 독립 µs는 측정 못 함이다. 수치 차이를 특정 단계의 추가 삭제 가능 비용이나 하위 계약 변경의 확정 효과로 배분하지 않는다.',
          '', '각 results.json의 submit_thread_cores × 1e6 / throughput_per_second를 먼저 계산한 뒤 RUNS=3 중앙값을 썼다. 각 열은 독립적으로 구한 3회 중앙값이므로 CPU 열 사이의 합계가 일치하지 않을 수 있다. 조건 B에서 source CPU 0–7, target CPU 8–15, payload 1024, warmup 30초, active 5초를 사용했다. 과거의 16.6 µs 대신 이번 main 재측정값을 비교 기준으로 삼았다. 측정은 measure.lock의 배타 잠금 아래 실행했고, 표의 값에는 application의 기존 생성·관찰·검증 비용이 포함된다.',
          '', '지정 pin.patch를 적용했다. 셸 &와 SIGKILL을 쓰지 않도록 run_local.sh의 프로세스 시작·종료 부분만 measure.py의 관리되는 subprocess로 대신했다. 동일한 client/server 인자와 runner_common.sh의 bench_run_cell을 사용했고 종료는 SIGTERM과 wait로 처리했다.',
          '', '| build | 실행별 caller µs/request |', '|---|---|']
for side, label in labels.items():
    p = s[side].get(pattern)
    values = ', '.join(f"{r['caller_us']:.3f}" for r in p['runs']) if p else '측정 못 함'
    lines.append(f'| {label} | {values} |')
lines += ['', '| backpressure CPU | source 전체 µs/request | non-submit µs/request |', '|---|---:|---:|']
for side in ['main', final_side]:
    p = s[side].get(pattern)
    if p:
        m = p['median']
        lines.append(f"| {labels[side]} | {m['source_us']:.3f} | {m['non_submit_us']:.3f} |")
    else:
        lines.append(f'| {labels[side]} | 측정 못 함 | 측정 못 함 |')
main_range = [r['caller_us'] for r in s['main'][pattern]['runs']]
final_range = [r['caller_us'] for r in f['runs']] if f else []
range_text = f"Main의 실행 범위는 {min(main_range):.3f}–{max(main_range):.3f} µs, 최종의 실행 범위는 {min(final_range):.3f}–{max(final_range):.3f} µs다." if final_range else ''
lines += ['', '단계별 독립 µs를 모두 측정하지는 않았다. 아래 차이는 서로 다른 빌드의 3회 중앙값 차이이며 개별 함수 시간이나 인과관계가 확정된 비용 분해가 아니다. ' + range_text + ' 특히 단계 묶음 사이의 작은 차이를 확정적인 독립 삭제 효과로 판정하지 않는다.',
          '', '| 비교 | caller 감소 µs/request |', '|---|---:|']
for a, b, label in [('main', 'step2', '1+2단계 묶음'), ('step2', 'step4', '3+4단계 묶음'), ('step4', 'final', '5단계')]:
    x, y = s[a].get(pattern), s[b].get(pattern)
    v = f"{x['median']['caller_us']-y['median']['caller_us']:+.3f}" if x and y else '측정 못 함'
    lines.append(f'| {label} | {v} |')
if final_side != 'final':
    v = s['final'][pattern]['median']['caller_us'] - s[final_side][pattern]['median']['caller_us']
    lines.append(f'| 후속 점검의 정적 작성 규칙 통합 | {v:+.3f} |')
lines += ['| 1단계 단독 | 측정 못 함 |', '| 2단계 단독 | 측정 못 함 |',
          '| 3단계 단독 | 측정 못 함 |', '| 4단계 단독 | 측정 못 함 |',
          '', '| request-serial 1024 | 평균 RTT의 3회 중앙값, µs | P95의 3회 중앙값, µs | req/s |',
          '|---|---:|---:|---:|']
for side in (['main', 'final', final_side] if final_side != 'final' else ['main', 'final']):
    p = s[side].get('request-serial')
    if p:
        m = p['median']
        lines.append(f"| {labels[side]} | {m['latency_mean_us']:.3f} | {m['latency_p95_us']:.3f} | {m['req_s']:,.3f} |")
    else:
        lines.append(f'| {labels[side]} | 측정 못 함 | 측정 못 함 | 측정 못 함 |')
lines += ['', '실행별 원본 결과는 log/<build>/run1–3/ 아래에 있고, 계산 결과와 경로는 summary.json에 있다. collect.py는 errors·abandoned·server rejected·target errors가 0인지, submitted=completed=target received인지, contaminated와 drain_bound_hit가 없는지 검사한다.',
          '', '리팩토링 점검은 요청한 다섯 항목을 기준으로 했다.',
          '', '| 단계 | 삭제한 메커니즘과 남은 부분 |', '|---|---|',
          '| 1 | 기존 node transport index 두 종류를 준비된 entry 한 map으로 합쳤다. Route의 두 번째 map 조회와 lane 왕복, request 경로의 result wrapper는 제거했다. 이름 map 조회와 variant 분기는 남았다. Declared-type serializer는 main부터 기존 1024개 bounded cache에서 첫 사용 때 선택하므로 새 절감으로 계상하지 않는다. |',
          '| 2 | 측정한 request 경로의 TLS header-plan matches와 정적 fragment 재선택을 제거했다. Type fragment는 기존 serializer state, channel fragment와 codec 함수는 bind index가 소유한다. Cache를 추가하지 않았다. |',
          '| 3 | JSON header의 별도 native message와 service frame으로의 header 복사를 제거했다. E1에서 typed encode·유효성·header 크기·flow snapshot을 확정하고, 기존 raw-owner pack 위치에서 canonical writer가 최종 service payload에 header bytes를 한 번 쓴다. Owned encoder가 기존 header message 표현을 대체하며 worker·queue·지연 admission은 없다. |',
          '| 4 | 측정한 request 경로의 raw bool task → host submit-result task → mesh observer를 제거했다. 기존 binding adapter에 최종 admitted source를 직접 넘긴다. 다른 경로가 쓰는 legacy wrapper는 유지한다. Target 거부 분류와 오류 mapping은 기존 소유 함수로 모았다. |',
          '| 5 | Mesh의 public-host forwarding 호출을 제거하고 기존 raw owner로 바로 연결했다. 추가했던 host forwarding 함수도 제거했다. Test probe를 켰을 때만 기존 fault injection 위치를 보존하는 예외 캡처가 있으며 production 빌드에는 들어가지 않는다. |',
          '', '점검 결과는 5항목 중 4항목 삭제, 1항목 부분 삭제다. 새 production 제어 flag·counter·queue·lock·poller·cache는 0개다. Native header와 part vector를 소유 encoder 표현으로 대체했고 기존 serializer state와 bind index를 재사용했다. Intrusive owner·pool·arena와 allocator 교체는 하지 않았다. 새 상태로 기존 control owner를 중복하지 않았다.',
          '', '후속 점검에서 정적 JSON fragment 작성의 중복 한 건을 발견해 기존 TLS plan과 bind plan이 같은 codec 작성 함수를 쓰도록 합쳤다. 정적 fragment 작성 위치는 2→1이다. 새 helper는 중복된 작성 코드를 대체했고 상태는 추가하지 않았다. 통합 전 측정과 binary를 보존했고 통합 후 최종 binary를 별도로 재측정했다. Bind·최초 사용 코드의 정리이므로 두 실행의 차이를 hot path의 확정 절감량으로 해석하지 않는다.',
          '', '수정 전/후 판정 위치 수는 C1 terminal 1→1, C2 deadline 1→1, physical admission·재제출 1→1, 최종 typed delivery 1→1이다. Admission 성공을 최종 admitted에 다시 반영하던 판정 위치는 binding adapter와 mesh observer 2→binding adapter 1로 줄었다. 정적 header-plan 선택은 해당 request 경로에서 TLS matching 1→0이고 bind/최초 serializer 준비가 소유한다.',
          '', '소유 계층은 Framework codec의 serialization, Framework registry의 C1/C2, 기존 dispatcher의 typed delivery, binding/Core의 physical admission·WRITABLE·retry다. Registry·lane·raw port·binding/Core 알고리즘은 바꾸지 않았다.',
          '', '근거 조항은 S01 §9–11, S08 §4.4 및 68–70·96–113, state lane §5, binding async-execution-model 117–122, S05 218–234다. E1 encode 실패, 최초 절대 deadline과 flow, 등록 후 native submit, 실제 admitted, logical terminal과 physical cleanup의 분리를 유지하는 변경이다.',
          '', '교차언어 대조는 기존 .NET·Java/Kotlin·Node의 registry terminal·deadline·독립 completion dispatcher 소유 경계를 정적으로 확인했다. C++만 소유권을 바꾸는 변경은 하지 않았다. 다른 언어의 runtime 실행과 교차언어 contract test는 측정 못 함이다. 원래 소유 위치의 언어별 근거는 cdes/report.md의 결정 소유 표에 있다.',
          '', '변경 분류는 A 계약 적응이다. cdes의 삭제 가능 단계 분류 C는 이 작업 지시의 분류이며 저장소의 우회 변경 분류 C와 다르다. Spec 개정이나 새 binding 기능을 구현한 결과가 아니다.',
          '', '각 단계의 release 빌드는 step1–step5 library·CMakeCache·diff와 함께 보존했다. 필수 빌드 명령은 cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0이었다. 최종 테스트 구성은 BUILD_TESTS·BUILD_FOUNDATION_TESTS를 켜고 samples·cross-language를 껐으며, Git 조회를 피하도록 CMAKE_DISABLE_FIND_PACKAGE_Git=ON의 기존 archive scan을 사용했다.']
test = j / 'ctest-final.log'
if test.exists():
    t = test.read_text()
    matches = re.findall(r'\d+% tests passed, \d+ tests failed out of \d+', t)
    lines += ['', '최종 C++ unit·contract 검사: ' + (matches[-1] if matches else '완료 요약 없음') + '.',
              '실행 명령은 measure-shared.sh ctest --preset linux-ninja-release -L \'^(unit|contract)$\' --output-on-failure이다. 전체 출력과 실패 내용은 ctest-final.log에 있다.']
    if '100% tests passed, 0 tests failed out of 297' in t:
        lines += ['', '남은 테스트 실패와 건너뛴 항목은 없다. 요청 범위의 미완료 부분은 1단계의 transport 이름 조회·variant 분기 삭제와 개별 1·2·3·4단계의 독립 µs 측정이다.']
else:
    lines += ['', '최종 C++ unit·contract 검사는 측정 못 함이다.']
wire_name = 'wire-' + final_side + '.log'
wire = j / wire_name
wire_ok = wire.exists() and 'wire byte identity: PASS, 12/12 frames' in wire.read_text() and 'intentional one-byte mutation: detected' in wire.read_text()
lines += ['', f'최종 wire byte-identity는 12/12 frame 일치로 통과했고 고의 한 byte 변조도 검출했다. 전체 결과는 {wire_name}에 있다.' if wire_ok else '최종 wire byte-identity는 측정 못 함이다.',
          'wire_check.cpp는 실제 BenchPayload 1 KiB와 고정 correlation·deadline으로 여섯 입력의 service header와 payload 12개 frame을 비교한다. Escaping·UTF-8 metadata·flow·topic/source·deadline 없음과 bound RouteChannel/service plan을 포함한다. 잘못된 metadata의 pack 전 거부, 폐기된 encoder 미실행과 고의 한 byte 변조 검출도 검사한다. 원본 main fixture와 main bytes를 보존했다.',
          '', 'Native 호출 수 검사는 별도 header frame 삭제에 맞춰 init 8→7, close 10→9, copy 2 유지를 요구한다. 관련 검사 두 항목이 통과했고 실제 8건에서 init 합계 56, copy 16, close 72를 확인했다. Test adapter 세 곳은 production과 같은 message_parts 소유 경로로 연결했다. Publication 테스트 한 파일은 삭제한 route_channels index를 nodes 표현으로 연결했고 공개 entry의 route 객체 일치 검사도 추가했다. 오류·timeout·completion 조건을 완화하지 않았다. 상세 결과는 ctest-request-detail.log에 있다.',
          '', 'Local C++ package는 기존 Core 1.23.0으로 build·install·verify했고 잘못된 Core 1.22.0 거부도 확인했다. 사용자 Git/Core/binding 수정 금지를 지키기 위해 parent package wrapper 대신 Git root 조회를 고정 경로로 대체한 C++ child script를 실행했다. 최종 package install은 기존 prefix를 보존해 unchanged file의 mtime을 유지했고 전후 library hash가 일치했다. 성능용 bench build는 TEST_PROBES=OFF다.',
          '', '메모리 감시가 성능용 빌드를 MemAvailable 12.955 GiB, 테스트용 빌드를 12.979 GiB에서 SIGTERM으로 중단한 뒤 순차 재개했다. 테스트 빌드는 요청한 --parallel 3 명령을 유지하고 CMake 컴파일 동시 수를 2로 제한했다. 이 설정은 실행 코드에 영향을 주지 않는다. 측정별 MemAvailable 표본은 memory-samples.jsonl에 보존했다. Lock은 기다렸고 SIGTERM/SIGINT 외 강제 종료나 셸 & 작업은 쓰지 않았다.',
          '', '변경 파일은 다음과 같다. 네 언어 run_local.sh 변경은 사용자가 지정한 pin.patch이고 runtime 변경은 C++뿐이다.', '']
diff = (j / 'prototype.diff').read_text()
for path in re.findall(r'^\+\+\+ b/(.+)$', diff, re.M):
    lines.append('- ' + path)
lines += ['', 'Core·binding·spec 파일 변경은 없다. PR·commit·push·GitHub 작업과 다른 model·sub-agent 호출은 하지 않았다. 최종 diff는 prototype.diff에 저장했다.',
          '', '사용자 지시와 달리 착수 때 scripts/dev/internal-docs.sh를 실행해 외부 docs 저장소의 git pull과 agent 정의 복사가 실행됐다. Git 제한을 놓친 실수였다. 초기 CMake 구성에도 read-only Git probe가 있음을 뒤에 확인했다. 최종 테스트 구성은 Git 조회가 없는 기존 archive scan 경로를 사용했다. 직접 Git 명령은 지정 pin.patch 적용과 diff 조회·저장만 사용했다.']
(j / 'report.md').write_text('\n'.join(lines) + '\n')
