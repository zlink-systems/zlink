1639 step 1 PR 4155 수정 보고

branch는 perf/1639-cpp-p1-receive-record이고 HEAD는 45ea3c721e다. 시작할 때 있던 21개 파일의 미커밋 diff를 유지하고 그중 7개 파일을 추가 수정했다. 빌드, 테스트, 음성 대조 실행과 lock 획득은 하지 않았다. 아래 할당·복사 수는 코드에서 산정한 값과 시험 기대값이며 실행 측정값이 아니다.

C는 framework/languages/cpp/framework/src/runtime/, T는 framework/languages/cpp/tests/Zlink.Framework.UnitTests/다. H는 HEAD, I는 이번 작업을 시작할 때의 미커밋 diff, W는 이번 수정 결과다. 전후 수는 H → I → W 순서다.

(1) 즉시 reply 비용과 deferred reply 보존

C/mesh/service_mailbox.hpp:153의 target 생성자를 explicit로 바꿨다. 같은 파일 :272의 service_mailbox_reply_view_t는 record 또는 retained target의 logical source, correlation, native RID, token을 참조한다. 생성자는 inline이고 네 멤버 모두 reference다. C/mesh/raw_mesh_node_owner.hpp의 reply 진입점 11개 선언은 view를 받는다. record마다 owning target을 만드는 경로가 없어졌다. record/target마다 reply overload를 추가하는 대안보다 선언과 reply 구현을 반복하지 않는 view를 선택했다.

소유 target을 만드는 제품 명령 위치는 C/mesh/service_mailbox.cpp:85의 reply_source() 한 곳으로 유지했다. target의 RID/token 복사 위치도 :76/:77 한 곳씩이다. 즉시 호출에서 이 위치에 암시적으로 진입할 수 있던 함수 선언 수는 0 → 11 → 0개다. deferred target의 소유 생성 위치 수는 1 → 1 → 1개이며, W에서는 명시적 retention만 이를 사용한다.

native RID/token이 있는 record의 즉시 reply 인자 수락 비용은 RID 복사 0 → 1 → 0회, token 복사 0 → 1 → 0회, logical vector 복사 0 → 0 → 0회, 할당 0 → 0 → 0회다. W의 reference view 생성·복사는 token shared owner를 증감시키지 않는다. native reply에 넘기는 source_rid()도 :290에서 const reference를 반환한다. H의 record.source_rid() 값 반환에 있던 별도의 native RID 복사 1회도 이 reply 경로에서 제거했다. 이 수는 Framework의 metadata 준비 비용이며 payload encode, lane과 binding reply builder 자체의 기존 비용은 포함하지 않는다.

N개 materialized parts와 nonempty logical source를 가진 수신 record의 reply_source()는 native message handle 복사 N → 0 → 0회, native RID 복사 1 → 1 → 1회, token 복사 1 → 1 → 1회, logical vector 복사 1 → 0 → 1회, C++ 할당 3 → 1 → 2회다. W의 할당은 shared target과 logical vector에 각 1회다. I에서 빠졌던 logical identity를 복원한 비용이며 H보다 할당이 1회 적다. retained target으로 실제 reply를 호출할 때 추가 RID/token 복사는 I와 W 모두 0회다. native context가 없는 local target은 RID/token을 만들지 않고 logical source와 correlation으로 local operation을 완료한다.

prepare_relocation_ready_reply()도 coroutine 인자로 view를 값으로 받아 reference 필드의 임시 view 수명 문제를 피한다. 유일한 제품 caller인 C/stateful/public_host_runtime.cpp:4470은 살아 있는 request를 co_await 동안 유지한다. 반환하는 deferred callable의 source RID 1회와 token 1회 복사는 H와 동일하다. I의 owning coroutine 인자에 추가됐던 복사는 제거했다. callable은 borrowed view를 capture하지 않는다.

T/test_cpp_framework_receive_record.cpp:365의 ImmediateReplyBorrowsMetadataWithoutEntryAllocationsOrCopies는 실제 target.reply(record, payload)를 호출한다. 계수 구간은 record 인자 준비부터 reply 함수 진입까지다. Linux linker wrapper(:114/:119/:132)가 진입 시 할당 수를 저장하고, native RID/token reference가 원본 객체와 다른 경우 각각 1개를 센다. 따라서 RID/token 수는 경계에 별도로 물질화된 metadata 객체 수이며 copy constructor 호출 횟수 전체를 hook하는 계수는 아니다. view의 trivial copy와 source_rid()의 const-reference 반환도 static_assert로 확인한다. 정상 기대값은 진입 1회, 할당/RID/token 각각 0회이고, wrapper는 실제 reply 구현을 계속 호출하여 caller completion까지 확인한다.

ZLINK_RECEIVE_RECORD_NEGATIVE=immediate-reply는 같은 구간에서 명시적 owning target을 만든다(:392). nonempty logical vector 할당 1회, 별도 RID/token 각 1개가 생겨 세 0회 단언을 깨뜨리는 구조다. serializer와 binding의 비용을 0이라고 주장하지 않는다. 정상·음성 대조 모두 미실행이다.

T/test_cpp_framework_receive_record.cpp:414의 ReplySourceRetainsOnlyTargetWithoutPartCopies는 할당 기대값을 2회로 조정했고 native message copy 0회 단언은 유지했다. logical identity의 값 보존과 별도 저장소도 확인한다(:446/:447). record 파괴·claim 해제 뒤 실제 reply 완료를 확인한다. 기존 reply-source 음성 대조는 계수 구간에서 두 parts를 복사하므로 할당 3회와 native copy 2회가 되어 실패하는 구조다. T/test_cpp_framework_claim_dispatch_cost.cpp:514는 logical identity와 native routing metadata를 각각 대조한다.

(2) reply guard의 identity

아래 위치는 모두 C/mesh/raw_mesh_node_owner.cpp다. correlation, token과 terminal 검사는 기존 의미를 유지한다. native RID는 reply 라우팅에만 사용하며 local.node_routing_id를 typed RID로 만드는 비교는 6 → 0개(I → W)다.

prepare_relocation_ready_reply():2020의 empty guard는 logical 1곳 → 삭제 → logical 1곳이다.
reply_relocation_failed():2034의 empty guard는 logical 1곳 → 삭제 → logical 1곳이다.
reply():2046의 empty guard는 logical 1곳 → 삭제 → logical 1곳이다.
reply_failure():2065의 empty guard는 logical 1곳 → native presence 1곳 → logical 1곳이다. :2070의 local-node 비교는 logical 1곳 → native 1곳 → logical 1곳이다.
reply_infrastructure():3051의 empty guard는 logical 1곳 → native presence 1곳 → logical 1곳이다. :3055의 local-node 비교는 logical 1곳 → native 1곳 → logical 1곳이다.
reply_user_spot_create():3097의 local-node 비교는 logical 1곳 → native 1곳 → logical 1곳이다. header :407의 편의 overload는 직접 identity를 판정하지 않고 이 구현으로 전달한다(0 → 0 → 0곳).
reply_actor_create():3137의 local-node 비교는 logical 1곳 → native 1곳 → logical 1곳이다.
reply_actor_join():3161의 empty guard는 logical 1곳 → 삭제 → logical 1곳이다.
reply_instance_spot_activation():3198의 remote/token guard와 :3205의 local completion guard는 각각 logical 1곳 → native 1곳 → logical 1곳이다. :3198의 조건 순서도 H와 같이 복원했다. 원래 없던 empty guard는 추가하지 않았다.
reply_user_spot_close():3221과 reply_bound_session_bind():2602는 직접 identity를 판정하지 않는다(각 0 → 0 → 0곳). reply_infrastructure()의 검사를 사용한다.

identity 판정 위치는 합계 12 → 8 → 12곳이다. logical 판정은 12 → 0 → 12곳, native 판정은 0 → 8 → 0곳이다. I에서 누락된 empty guard 4곳을 복원한 수치다. H에서 W까지 기존 guard의 identity를 바꾼 곳은 0곳이다. 위 10개 reply 함수의 본문 전체를 git show HEAD와 대조했으며 문자열 단위로 동일하다. 달라진 signature는 record 대신 view를 받아 같은 판정을 수행한다. target의 native RID가 없을 때 logical RID를 native metadata로 대체하던 분기는 1 → 0곳(I → W)이며, target은 두 identity를 구분해 보존한다.

T/test_cpp_framework_receive_record.cpp:463은 native RID/token이 있어도 logical source가 빈 경우를 record와 retained target으로 각각 확인한다. 여섯 reply 진입 경로의 거절/예외 의미를 검사한다. :500은 native RID와 logical local-node ID가 다른 record를 만들고 다섯 reply 경로를 각각 record/target으로 호출하여 등록한 local operation이 제거되는지 확인한다. :555는 native context가 없는 local target의 실패 reply도 확인한다. 모두 미실행이다.

소유 계층은 Framework의 logical empty/local-operation 판정과 metadata 수명 보존이다. native reply 라우팅과 token/socket 검증은 기존 Core·binding이 계속 소유한다.
스펙 근거는 framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md §4.3 W4, bindings/doc/spec/async-coroutine-policy.ko.md §5와 bindings/doc/spec/cpp/README.ko.md:563이다. binding received.hpp의 RID/token const-reference accessor를 사용한다.
교차언어 대조에서는 .NET framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/FrameworkServiceTypes.cs:835/:946/:948의 source identity와 reply delegate 분리, Java framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:7527/:7544의 inbound source/requestSequence reply, Node framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:924/:940의 localReply/opaque reply 경계를 확인했다. Kotlin은 Java runtime을 공유한다. 이번 수정은 C++의 미커밋 회귀를 복원하며 다른 언어의 계약이나 구현을 바꾸지 않는다. 교차언어 시험은 실행하지 않았다.
변경 분류는 B다. 감독이 지적한 기존 미커밋 diff의 결함을 수정했다. 새 reply 정책은 추가하지 않았다.

(3) 빈 body_parts()와 caller 전제

C/mesh/service_mailbox.hpp:224는 parts가 비었으면 빈 span을 반환하고, parts가 있으면 subspan(1)을 반환한다(:227/:228). prefix만 있는 경우도 유효한 빈 span이다. T/test_cpp_framework_receive_record.cpp:602의 BodyPartsWithoutPrefixAreEmpty는 default record, prefix만 있는 record, 본문 1개, move 뒤 source, take_parts() 뒤와 replace_parts({}) 뒤를 확인한다. 본문이 있으면 size 1을 확인하므로 항상 빈 span을 반환하는 구현도 거부한다. 시험은 미실행이다.

제품 caller는 네 곳이다. C/client_server/client_server_location_runtime.cpp:1418의 [0] 앞에는 :1410의 parts.size() == 2 검사가 있다. C/mesh/raw_mesh_node_owner.cpp:3301의 [0] 앞에는 :3280의 parts.size() >= 2 검사가 있다. C/stateful/public_host_runtime.cpp:8326의 back()과 :8425의 [0]은 :8306의 parts.size() < 2 거절 뒤에 실행된다. 두 원소 이상이라는 전제는 유지되므로 caller에 새 조건을 추가하지 않았다.

기존 시험 caller는 T/test_cpp_framework_actor_maintenance_join.cpp:3899/:3944/:4337, T/test_cpp_framework_relocation_seal_contract.cpp:226/:248/:3718, T/test_cpp_framework_receive_record.cpp:584다. actor_record():3592와 spot_send():190의 helper가 service prefix와 application payload 두 parts를 만든다. receive-record의 해당 caller도 from_parts()에 두 parts를 명시한다. 모든 back()/[0]이 본문 존재를 전제하며 이번 변경은 그 전제를 완화하지 않는다. 새 시험의 empty() 호출에는 본문 존재 전제가 없다.

PM의 원래 요청도 유지했다. parts()는 const이고 replace_parts()가 cache 두 개를 한 곳에서 지우며 take_parts()도 이 경로를 쓴다. 분류 판정 위치는 1 → 1곳(H → W)이고 cache 무효화 지시 위치도 시험의 직접 reset 1곳에서 제품 1곳으로 이동한 상태다. classification 음성 대조는 공개 parts 교체로 재분류를 유발하는 기존 구조를 유지했다. reply_source()는 parts를 보존하지 않으며 raw_mesh_node_owner_test_access_t friend도 한 곳이다.

(4) 변경 줄의 괄호와 인자 대조

이번 작업 시작 시 저장한 7개 파일과 최종 파일을 비교했다. 변경 줄과 겹치는 C++ 호출·선언 275곳을 추출하여 선언의 필수/기본 인자 수와 대조했다. comment·문자열을 제외한 (), [], {}의 짝과 중첩도 확인했다. 불일치는 0건이다. 이는 컴파일·타입 검사 결과가 아니다. 아래 목록은 이번 작업의 변경 줄과 겹치는 표현만 포함하며 이전 세션의 변경 목록을 반복하지 않는다.

n은 호출·선언 인자 수이고 p/s/b는 해당 호출·선언 인자 표현의 ()/[]/{} 쌍 수이며 바깥 ()도 포함한다. 정의의 본문은 이 수에 넣지 않으며 본문 중첩은 별도로 확인했다. 같은 이름과 수가 같은 위치를 묶었다. 같은 줄의 두 호출은 ×2로 표시한다. ReplyReal과 ReplyWrap은 :114/:119의 __real__/__wrap__ reply ABI 이름이다.

reply/relocation/infrastructure/close는 인자 2개, reply_failure는 3개다. User Spot create와 Actor create는 필수 2개/전체 3개, Instance Spot activation은 필수 3개/전체 4개, Actor join은 필수 5개/전체 8개다. record/target → view는 모두 생성자 인자 1개다. register_local_operation_async는 필수 2개/전체 4개, record.from_parts는 필수 3개/전체 9개다. 이 선언과 모든 변경 호출의 순서·기본 인자를 확인했다. C wrapper는 this pointer가 명시적 첫 인자이므로 인자 3개이며, c++filt로 CMake의 심볼이 reply(view, const application_payload_t&)임을 확인했다.

framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp의 변경 표현을 다음과 같이 확인했다.
2017 prepare_relocation_ready_reply: n=2, p=1, s=0, b=0.
2020/2034/2046/2065/3051/3161 empty: n=0, p=1, s=0, b=0.
2020/2034/2046/3161/3198 reply_token: n=0, p=1, s=0, b=0.
2031 reply_relocation_failed: n=2, p=1, s=0, b=0.
2043 reply: n=2, p=1, s=0, b=0.
2061 reply_failure: n=3, p=1, s=0, b=0.
3048 reply_infrastructure: n=2, p=1, s=0, b=0.
3074 reply_user_spot_create: n=3, p=1, s=0, b=0.
3122 reply_actor_create: n=3, p=1, s=0, b=0.
3151 reply_actor_join: n=8, p=1, s=0, b=0.
3182 reply_instance_spot_activation: n=4, p=1, s=0, b=0.
3221 reply_user_spot_close: n=2, p=1, s=0, b=0.

framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.hpp의 변경 표현을 다음과 같이 확인했다.
253 reply: n=2, p=1, s=0, b=0.
255 reply_failure: n=3, p=1, s=0, b=0.
403/407 reply_user_spot_create: n=3, p=1, s=0, b=0.
416 reply_actor_create: n=3, p=1, s=0, b=0.
420 reply_instance_spot_activation: n=4, p=1, s=0, b=0.
425 reply_user_spot_close: n=2, p=1, s=0, b=0.
454 prepare_relocation_ready_reply: n=2, p=1, s=0, b=0.
456 reply_relocation_failed: n=2, p=1, s=0, b=0.
468 reply_actor_join: n=8, p=1, s=0, b=0.
688 reply_infrastructure: n=2, p=1, s=0, b=0.

framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp의 변경 표현을 다음과 같이 확인했다.
74 source_routing_id: n=1, p=1, s=0, b=0.

framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp의 변경 표현을 다음과 같이 확인했다.
153 service_mailbox_reply_target_t: n=1, p=1, s=0, b=0.
226 parts: n=0, p=1, s=0, b=0.
227 empty: n=0, p=1, s=0, b=0.
228 span: n=1, p=1, s=0, b=0.
228 subspan: n=1, p=1, s=0, b=0.
275/282 service_mailbox_reply_view_t: n=1, p=1, s=0, b=0.
276/283 source_routing_id: n=1, p=1, s=0, b=0.
277/284 correlation: n=1, p=1, s=0, b=0.
278/285 _routing_id: n=1, p=2, s=0, b=0.
278/285 routing_id: n=0, p=1, s=0, b=0.
279/286 _token: n=1, p=2, s=0, b=0.
279/286/289 reply_token: n=0, p=1, s=0, b=0.
290 source_rid: n=0, p=1, s=0, b=0.
290 value: n=0, p=1, s=0, b=0.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp의 변경 표현을 다음과 같이 확인했다.
514 require: n=2, p=7, s=0, b=0.
515×2 routing_id: n=0, p=1, s=0, b=0.

framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_receive_record.cpp의 변경 표현을 다음과 같이 확인했다.
114/132 ReplyReal: n=3, p=1, s=0, b=0.
119 ReplyWrap: n=3, p=1, s=0, b=0.
127/128/129/130 addressof: n=1, p=2, s=0, b=0.
127/371 source_rid: n=0, p=1, s=0, b=0.
128/379/471/508/511/564 routing_id: n=0, p=1, s=0, b=0.
129/130/380/472/565 reply_token: n=0, p=1, s=0, b=0.
365/463/500/555 TEST_F: n=2, p=1, s=0, b=0.
367/369 static_assert: n=1, p=1, s=0, b=0.
370 static_assert: n=1, p=4, s=0, b=0.
371 decltype: n=1, p=3, s=0, b=0.
371 declval: n=0, p=1, s=0, b=0.
373/465/502 request: n=0, p=1, s=0, b=0.
374/466/503 begin_receive_turn: n=0, p=1, s=0, b=0.
375/467/504/610/613/617 ASSERT_EQ: n=2, p=3, s=0, b=0.
375/467/504 receive_next: n=2, p=2, s=0, b=0.
375/407/408/467/493/494/504/515/548/549/558 now: n=0, p=1, s=0, b=0.
376/468/505 take: n=0, p=1, s=0, b=0.
377/406/469/492/506/547 mailbox: n=0, p=1, s=0, b=0.
377/469/506 take: n=1, p=1, s=0, b=1.
378/470/507/518/559 ASSERT_TRUE: n=1, p=1, s=0, b=0.
379/380/471/472/508 ASSERT_TRUE: n=1, p=2, s=0, b=0.
381 ASSERT_FALSE: n=1, p=2, s=0, b=0.
381/605/607/612/615/619 empty: n=0, p=1, s=0, b=0.
382/476/546 payload: n=0, p=1, s=0, b=0.
383 fault: n=0, p=1, s=0, b=0.
384 addressof: n=1, p=1, s=0, b=0.
385 exchange: n=2, p=1, s=0, b=0.
387 decltype: n=1, p=1, s=0, b=0.
387 scope: n=2, p=1, s=0, b=0.
389 count_allocations: n=1, p=5, s=1, b=3.
392 copied: n=1, p=1, s=0, b=0.
393/395/478/491 reply: n=2, p=1, s=0, b=0.
398 reset: n=0, p=1, s=0, b=0.
399 EXPECT_TRUE: n=1, p=1, s=0, b=0.
400/401/402/403/443/446 EXPECT_EQ: n=2, p=1, s=0, b=0.
404 printf: n=5, p=1, s=0, b=0.
406/492/547 release: n=1, p=1, s=0, b=0.
408/410/411/494/496/497/549/551/552 load: n=1, p=1, s=0, b=0.
409/495/550 progress: n=1, p=1, s=0, b=0.
410/411/496/497/551/552 EXPECT_EQ: n=2, p=2, s=0, b=0.
447 EXPECT_NE: n=2, p=3, s=0, b=0.
447×2 data: n=0, p=1, s=0, b=0.
473 clear: n=0, p=1, s=0, b=0.
474/520/563 reply_source: n=0, p=1, s=0, b=0.
478/485/540/564/565/567 EXPECT_FALSE: n=1, p=2, s=0, b=0.
479 EXPECT_FALSE: n=1, p=2, s=0, b=1.
479 reply_relocation_failed: n=2, p=1, s=0, b=1.
480 EXPECT_FALSE: n=1, p=4, s=0, b=1.
480 prepare_relocation_ready_reply: n=2, p=1, s=0, b=1.
481/516/558 result: n=0, p=1, s=0, b=0.
482/517/558 value: n=0, p=1, s=0, b=0.
483/484 EXPECT_THROW: n=2, p=2, s=0, b=0.
483/525/566 reply_failure: n=3, p=1, s=0, b=0.
484 reply_user_spot_close: n=2, p=1, s=0, b=0.
485 reply_actor_join: n=7, p=1, s=0, b=0.
488/489 check: n=1, p=1, s=0, b=0.
490/510/545/562 startup_descriptor: n=0, p=1, s=0, b=0.
491/525/538/566 EXPECT_TRUE: n=1, p=2, s=0, b=0.
511 ASSERT_NE: n=2, p=3, s=0, b=0.
511 to_bytes: n=0, p=1, s=0, b=0.
515/558 register_local_operation_async: n=2, p=3, s=1, b=1.
521/522 service_mailbox_reply_view_t: n=1, p=1, s=0, b=0.
527 EXPECT_TRUE: n=1, p=2, s=0, b=1.
527 reply_user_spot_close: n=2, p=1, s=0, b=1.
530 EXPECT_TRUE: n=1, p=2, s=0, b=2.
530 reply_user_spot_create: n=2, p=1, s=0, b=2.
534 EXPECT_TRUE: n=1, p=2, s=0, b=3.
534 reply_actor_create: n=2, p=1, s=0, b=3.
538 reply_instance_spot_activation: n=3, p=1, s=0, b=0.
540/567 operation_pending: n=1, p=1, s=0, b=0.
541/568 unregister_local_operation: n=1, p=1, s=0, b=0.
546/605/607/612/615/619 EXPECT_TRUE: n=1, p=3, s=0, b=0.
546 reply: n=2, p=2, s=0, b=0.
560 from_parts: n=5, p=2, s=0, b=1.
602 TEST: n=2, p=1, s=0, b=0.
605/607/610/612/613/615/617/619 body_parts: n=0, p=1, s=0, b=0.
606 replace_parts: n=1, p=3, s=0, b=1.
606/609 from: n=1, p=2, s=0, b=0.
606/609 encode_node_send_header: n=0, p=1, s=0, b=0.
608 replace_parts: n=1, p=4, s=0, b=1.
609 from: n=1, p=1, s=0, b=0.
610/613/617 size: n=0, p=1, s=0, b=0.
611/616 move: n=1, p=1, s=0, b=0.
614 take_parts: n=0, p=1, s=0, b=0.
616 replace_parts: n=1, p=2, s=0, b=0.
618 replace_parts: n=1, p=1, s=0, b=1.

익명 함수 선언도 확인했다. T/test_cpp_framework_receive_record.cpp:386은 인자 1개, () 1쌍, capture [] 1쌍이다. :389는 인자 0개와 capture [] 1쌍이며 parameter ()는 생략됐다. :477은 인자 1개, () 1쌍, capture [] 1쌍이다. :515/:558은 각각 인자 2개, () 1쌍, capture [] 1쌍이다. 각 lambda 본문의 {}는 1쌍이며 내부 중첩은 별도로 대조했다.

aggregate 초기화도 확인했다. 같은 시험 :384의 counts는 5개 멤버 중 원본 pointer 1개를 지정하고 나머지 네 count는 기본값 0이다(p=1,s=0,b=1). :475의 close_reply는 외부 인자 2개와 header 인자 3개이며 {} 2쌍이다. :523의 header는 인자 3개와 {} 1쌍이다. :528의 close reply는 인자 2개와 {} 1쌍, :531의 create reply는 인자 4개와 {} 2쌍, :535의 actor create reply는 인자 5개와 {} 3쌍이다. :479/:480의 relocation aggregate는 각각 {} 1쌍의 기본 초기화다. C/mesh/service_mailbox.hpp:227의 빈 span도 인자 0개와 {} 1쌍이다.

framework/languages/cpp/CMakeLists.txt:3066의 target_link_options는 target, PRIVATE, wrap 옵션 세 개로 총 항목 5개이며 () 1쌍이다. 새 옵션은 :3069 한 줄이다. 기존 Linux/static/test-probe 조건을 유지했다.

(5) format과 남은 검증

bash scripts/format/format.sh --check cpp는 종료 코드 0으로 통과했다. git diff --check도 종료 코드 0으로 통과했다. 빌드·테스트·음성 대조 실행 결과는 없다. PM의 컴파일과 실행 검증이 남았다.

추가 수정 파일은 framework/languages/cpp/CMakeLists.txt, C/mesh/raw_mesh_node_owner.hpp와 .cpp, C/mesh/service_mailbox.hpp와 .cpp, T/test_cpp_framework_receive_record.cpp, T/test_cpp_framework_claim_dispatch_cost.cpp다. spec 파일과 나머지 기존 diff는 수정하지 않았다.

(6) 리팩토링 점검

reply guard 수정 전/후 규칙 수는 H → W 12 → 12곳이다. I에서 사라진 4곳은 복구했다. target의 native/logical 대체 규칙은 I → W 1 → 0곳이고, 즉시 호출의 owning 변환 허용 진입점은 11 → 0개다. 새 queue, lock, timer, flag와 thread는 추가하지 않았다. target의 logical vector는 H의 reply-source record가 보유하던 logical snapshot을 target으로 이동한 것이다. product의 test-only API도 추가하지 않았다. test 계측은 시험 소스와 linker wrapper에만 있다.

추가로 native/logical identity를 같은 값으로 취급할 수 있는 아래 8개 위치를 확인했다. 이번 승인 범위의 reply guard identity는 그대로 복원했고, 아래 위치의 identity 변경은 수행하지 않았다. 같은 이슈에서 판단할 근거로 보고한다.

C/mesh/service_mailbox.hpp:251의 record.source_rid()는 native가 있으면 native, 없으면 logical을 typed RID로 바꿔 반환한다. reply view는 이 혼합 helper를 사용하지 않지만 아래 node-identity 소비자는 여전히 사용한다.
C/mesh/service_mailbox.hpp:219의 from_received()는 native RID bytes를 logical source_routing_id의 초기값으로 사용한다. 호출자가 별도 logical identity로 교체하는 조건을 확인해야 한다.
C/mesh/raw_mesh_node_owner.cpp:3660은 native RID로 topology.peer()를 조회하고 :3661에서 descriptor logical ID 또는 native bytes를 logical source에 넣는다. service_topology_registry.cpp:515의 peer()는 logical node key를 조회하므로 두 RID가 다른 ingress의 admission 기준을 별도로 검토해야 한다.
C/stateful/public_host_runtime.cpp:6304는 혼합 source_rid() 결과를 session_owner_rid로 두고 :6308의 Session owner/location 조회에 사용한다. native route와 logical owner ID가 다를 때 조회 identity를 검토해야 한다.
C/stateful/public_host_runtime.cpp:8006은 혼합 source_rid() 결과를 receive_record의 source_node_rid로 기록한다. node identity와 native reply identity를 분리할 필요를 검토해야 한다.
C/stateful/public_host_runtime.cpp:8365도 같은 방식으로 application receive_record의 source_node_rid를 채운다.
C/stateful/public_host_runtime.cpp:8331은 혼합 source_rid() 결과를 local multicast의 source_node로 넘긴다. :9425는 이 값을 receive_record.source_node_rid에 기록한다.
C/stateful/public_host_runtime.cpp:6669는 혼합 source_rid()를 flow event의 source_rid 문자열로 기록한다. 이 필드가 logical source인지 native route인지 관찰 계약과 대조해야 한다.

ClientServer reply의 C/client_server/raw_client_server_owner.cpp:858/:886과 Channel reply의 C/channels/channel_host_service.cpp:416/:418도 확인했다. 이 위치는 empty source 검사와 native reply context 보존을 각각 사용하며 MeshNode local-ID 비교를 하지 않는다. 위 8개 항목처럼 node identity의 혼용으로 확정하지 않았다. 새 execution gate 우회나 중복 동기화는 추가하지 않았다. 추가 점검 항목은 발견 8곳, 이번 추가 identity 변경 0곳, 판단을 위해 보고한 위치 8곳이다.
