import datetime,json,pathlib,subprocess
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868');repo=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
refs=json.loads((job/'raw/main-reference.json').read_text())
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
def rate(row):
    n=row['median_msg_s']
    p=next(path for path in row['raw'] if json.loads(pathlib.Path(path).read_text())['cells'][0]['throughput_per_second']==n)
    return f'[{n:,.0f}]({p})'
live={}
if (job/'raw/partial-summary.json').exists():
    candidate=json.loads((job/'raw/partial-summary.json').read_text())
    if candidate['commit']==head:live=candidate
def live_rate(stat):
    if 'median_msg_s' in stat:return f"[{stat['median_msg_s']:,.0f}]({stat['median_raw']})"
    n=stat.get('runs',0)
    return f"[미완료 {n}/3](raw/partial-summary.json)" if n else '미측정'
rows=['| 패턴 | 크기 | L0 / 동시 binding msg/s | L0÷binding · 0.95 | main L8 / 당시 binding msg/s | main÷binding · 0.95 |','|---|---|---:|---|---:|---|']
for r in refs:
    verdict='미달' if r['ratio']<.95 else '통과'
    if r['zlink-cpp']['g5_status']!='pass':verdict+=' · G5 미통과, 수치 참고'
    cell=live.get('matrix',{}).get(f"{r['pattern']}-{r['payload_size']}",{})
    fw=cell.get('zlink-framework-cpp',{});bind=cell.get('zlink-cpp',{})
    judgement='판정 불가'
    if 'median_msg_s' in fw and 'median_msg_s' in bind:
        ratio=fw['median_msg_s']/bind['median_msg_s']
        judgement=f"[{ratio:.4f}](raw/partial-summary.json) · {('통과' if ratio>=.95 else '미달 · 비용 진단 필요')}"
        if fw['g5']!='pass' or bind['g5']!='pass':judgement+=' · G5 미통과'
    rows.append(f"| {r['pattern']} | {r['payload_size']//1024}K | {live_rate(fw)} / {live_rate(bind)} | {judgement} | {rate(r['zlink-framework-cpp'])} / {rate(r['zlink-cpp'])} | [{r['ratio']:.4f}](raw/main-reference.json) · {verdict} |")
inv=(job/'raw/disabled-code-table-owner.md').read_text()
text=f'''# #4350 C++ L0 — 진행 중

**완료 기준은 아직 판정할 수 없다.** L0와 동시 binding의 유효 측정값이 없다. 현재 `{head}`의 wire 검사 수정 빌드가 외부 측정의 exclusive lock·공통 gate를 기다리고 있다. [진행 기록](progress.md), [수정 빌드 로그](raw/build-l0-wire-final.log)가 현재 상태를 담는다. 이 보고서는 측정 완료 보고가 아니다.

실험 브랜치는 `exp/3868-cpp-ladder`다. 변경은 로컬에만 커밋했고 push·PR·merge는 하지 않았다. 앞선 313a7ccccf는 빌드·wire·bench 검사를 통과했으나 L0 source startup에서 immutable descriptor 검사를 위반해 측정이 중단됐다. [실패 원본](raw/L0/send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/source.log), [당시 실행 로그](raw/l0-corrected-driver-313.log)를 보존했다. owner를 preparing으로 만들고 실제 bind endpoint를 게시하며 serving으로 전환하도록 고쳤다. raw/L0 첫 시도는 판정에서 제외하고 raw/L0-owner에서 새로 측정한다. [첫 빌드 성공 원본](raw/build-l0-corrected-final.log)과 [이전 wire 검사](raw/verify-build-corrected.txt)도 보존했다. raw benchmark의 legacy envelope에는 Framework format marker가 없는데, 검사가 이를 Framework codec으로 읽었다. 현재 코드는 L0의 binding byte 일치 검사와 Framework wire의 codec 검사를 나누며 양쪽 손상 입력 거부도 검사한다. 수정 빌드가 완료된 뒤 다시 검증한다.

PM 08:30 UTC 결정에 따라 main을 재측정하지 않는다. 아래 main L8은 `d3fd955131`의 [r4190w 기준선](../r4190w/report.md)과 raw다. 기존 control 관련 파일은 [사용 제외 목록](raw/not-used-control.txt)으로 남겼다. 09:00 UTC 정정에 따라 L0 수신은 binding과 같은 protobuf decode를 수행하고, 응답은 새 envelope Message·새 BenchPayload body 복사·protobuf encode·새 body Message를 만든 뒤 같은 2-part binding reply submit으로 보낸다. 요청 part를 응답에 재사용하지 않는다. [응답 작업 검사기](tools/reply-parity.py)가 매 실행 전 두 코드를 file:line으로 나란히 progress.md에 기록하며 작업이 일치하지 않으면 실행을 중단한다.

측정 조건은 source CPU 0–7 / target CPU 8–15, warmup 30초 / active 5초 / 3회 중앙값이다. 이 조건의 새 L0/binding 실행은 아직 없다. [run-group.sh](tools/run-group.sh)는 r4190w 사본에서 JOB/raw 경로만 바꿨고 exclusive measure lock을 사용한다. C++ 빌드는 지정된 cpp-build-slot.sh / Release / -j3 / bindings source ON / Core 1.25.0 prefix / 이 worktree의 build-l3868을 사용한다. [빌드·검증·측정 연쇄 명령](tools/measure-corrected.sh), [출처 검사기](tools/provenance.py)는 이 worktree의 소스·바이너리 SHA-256과 commit·dirty·base를 확인한다. 검증 전 바이너리로 측정한 실행은 없다.

{chr(10).join(rows)}

기준선의 Core/binding metadata는 1.24.0이고, 이번 지정 prefix와 binding source는 1.25.0이다. main 표는 PM이 지정한 역사 참조이며 버전이 같은 새 control로 해석하지 않는다. [기준선 metadata](../r4190w/raw/bench/cpp/run1/zlink-cpp-send-saturation-1024/results.json).

각 처리량 링크는 중앙값에 해당하는 원시 실행이다. 모든 반복의 경로는 [기준선 집계](raw/main-reference.json)에 있다. main send 4K는 원래 binding의 G5 편차 초과로 수치 참고만 가능하다. main의 비율을 새 L0의 동시 binding 분모로 사용하지 않는다.

| L0 교차 시험 · send-saturation 1K | msg/s | 상태 |
|---|---:|---|
| raw 서버 + raw client | — | 미측정 |
| Framework 서버 + raw client | — | 미측정 |
| Framework 서버 + Framework client | — | 미측정 |

실제 클래스는 raw_mesh_node_owner_t다. start로 만든 실제 socket·poller를 ablation_send / ablation_request / ablation_receive가 사용한다. owner는 readiness에서 대기하고 EAGAIN까지 drain한다. 완료는 binding admitted/reply를 기존 bench observer로 관찰한다. 고정 handler는 Framework handler 활성화·DI·dispatcher의 실험 대체물이며 route_handler_invoker를 실행한 것으로 표기하지 않는다. optional raw-mesh peer의 admission/liveness adapter도 모사한 부분이며 기본 L0 matrix에서는 사용하지 않는다. canonical wire codec과 reply writer는 실제 Framework 클래스를 호출한다.

복원 단계별 제외·대체 위치는 다음과 같다. 중간 level은 실행을 거부하며, 아래 소유자를 순서대로 복원하고 검증해야 한다. L1–L7을 구현 완료한 결과로 표기하지 않는다. 현재 위치 원본은 [disabled-code-owner.json](raw/disabled-code-owner.json), 이전 f539 위치 목록은 [disabled-code.json](raw/disabled-code.json)에 보존했다.

{inv}

소유 계층: 물리 admission·HWM 재시도·native completion은 binding/Core이며 L0는 이를 재구현하지 않는다.

스펙 근거: [Messaging hot path §3·§4]({repo}/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:66), [Application job queue]({repo}/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:82), [Submit과 completion]({repo}/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:1).

교차언어 대조: 사용자가 지정한 C++ 단독 L0 실험이며 타 언어 runtime·public spec을 수정하지 않았다.

변경 분류: C(사용자가 승인한 실험 전용 기능 우회)다. 제품의 A/B 수정·계약 준수를 주장하지 않는다.

수정 전/후 물리 admission·재시도 판정 위치 수는 1→1(binding/Core)이다. 제외한 queue·worker·activation·operation 상태는 위 복원 목록에 기록했다. 인접 호출 경로 점검에서 별도 제품 수정으로 넘긴 항목은 없다.

남은 비용은 **미측정**이다. L0에도 binding과 같은 payload encode/decode·새 응답 생성·native admission/completion, bench 검증·counter, 실제 owner poll/receive가 남는다. 등록된 idle wake timer도 있다. 유효 셀이 없어 추가 Framework 비용과 비율 미달 원인을 profile로 판정하지 않았다. L0가 binding의 0.95 이상이라는 주장은 하지 않는다.

남은 검증은 수정 wire 검사·관련 bench 검사, L0/binding 전체 셀 및 L0 교차 시험이다. 첫 유효 send 1K 값이 생기는 즉시 progress.md에 기록한다. 작업 종료 시 C++ build directory를 정리하며 원시 파일은 보존한다.

기록 시각: {datetime.datetime.now(datetime.timezone.utc).isoformat()}.
'''
if (job/'raw/build-provenance.json').exists() and json.loads((job/'raw/build-provenance.json').read_text())['commit']==head:
    text=text.replace('현재 `'+head+'`의 wire 검사 수정 빌드가 외부 측정의 exclusive lock·공통 gate를 기다리고 있다.', '현재 `'+head+'`의 수정 빌드·wire 검사·관련 bench 검사 6개가 통과했고 L0/binding 측정이 진행 중이다. [검사 결과](raw/bench-tests-owner.txt), [wire·출처 검증](raw/verify-build-owner.txt), [빌드 출처·SHA-256](raw/build-provenance.json), [실행 로그](raw/l0-corrected-driver.log).')
    text=text.replace('수정 빌드가 완료된 뒤 다시 검증한다.', '수정 후 [wire 검사](raw/verify-build-owner.txt)와 [bench 검사](raw/bench-tests-owner.txt)를 모두 통과했다.')
    text=text.replace('남은 검증은 수정 wire 검사·관련 bench 검사, L0/binding 전체 셀 및 L0 교차 시험이다.', '남은 검증은 L0/binding 전체 셀 및 L0 교차 시험이다.')
if live:
    text=text.replace('L0와 동시 binding의 유효 측정값이 없다.', '전체 셀은 아직 완료되지 않았다. 아래 표는 검증을 통과한 3회 중앙값만 반영하며, 반복이 부족한 셀은 미완료로 표시한다.')
    for group,label in [('raw-raw','raw 서버 + raw client'),('framework-raw','Framework 서버 + raw client'),('framework-framework','Framework 서버 + Framework client')]:
        stat=live.get('cross',{}).get(group,{})
        text=text.replace(f'| {label} | — | 미측정 |',f'| {label} | {live_rate(stat)} | '+('3회 완료' if 'median_msg_s' in stat else '미완료')+' |')
    text=text.replace('유효 셀이 없어 추가 Framework 비용과 비율 미달 원인을 profile로 판정하지 않았다.', '완료되지 않은 셀과 비용 profile의 남은 범위를 포함해 추가 Framework 비용을 최종 판정하지 않았다.')
(job/'report.md').write_text(text)
