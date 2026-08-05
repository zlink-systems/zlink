#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:---check}"

node - "$repo_root" "$mode" <<'NODE'
'use strict';

const fs = require('fs');
const path = require('path');

const root = process.argv[2];
const mode = process.argv[3];
if (!['--check', '--self-test'].includes(mode)) {
  process.stderr.write('usage: verify-framework-instance-spot-contracts.sh [--check|--self-test]\n');
  process.exit(2);
}

const {readExactContract} = require(
  path.join(root, 'scripts/lib/framework-contract-documents.cjs'));
const languages = ['dotnet', 'cpp', 'java', 'kotlin', 'node'];
const tags = {
  dotnet: ['csharp'],
  cpp: ['cpp'],
  java: ['java'],
  kotlin: ['kotlin', 'java'],
  node: ['ts', 'typescript'],
};

const read = relative => {
  const absolute = path.join(root, relative);
  if (!fs.existsSync(absolute)) throw new Error(`missing contract document: ${relative}`);
  return fs.readFileSync(absolute, 'utf8');
};
const normalized = source => source.replace(/\s+/gu, ' ').trim();

const formalFixtures = [
  {
    path: 'framework/doc/framework/common/spec/06-framework-api.ko.md',
    required: [
      'actor-free Instance Spot factory',
      'Instance Spot은 actor-free lifecycle을 사용하며 Actor handler, Actor membership과 Logical Multicast subscription을 등록할 수 없다.',
      'Actor manager와 User Spot manager는 global ID를 받는 `Create`, `GetOrCreate`, `Find` family를 제공한다.',
      'Instance Spot은 manager create family를 제공하지 않는다.',
      'Instance intent를 명시한 경우에만 Missing authority의 cold activation을 시작한다.',
      '선택한 Mesh의 serving descriptor에 등록된 distinct Instance type이 하나일 때만 자동 선택한다.',
      'Source는 owner claim이나 reservation을 먼저 만들지 않는다.',
      '확보한 runtime만 factory와 initialize를 실행하고, activation envelope의 message를 durable activation inbox의 첫 record로 확정한다.',
      'Public object handle, directory, resolver와 unbounded list는 제공하지 않는다.',
      '| exact SpotRef close |',
    ],
  },
  {
    path: 'framework/doc/framework/common/spec/12-spot-messaging.ko.md',
    required: [
      '### 3.2 Instance Spot이 없을 때 새로 준비하기',
      'Spot direct call에 `Instance intent`',
      'target Spot이 존재하지 않으면 `NotFound`로 끝난다.',
      '서로 다른 type이 하나뿐이면 그 type을 자동으로 선택한다.',
      '최초 application message와 Spot 생성·reply에 필요한 정보를 하나의 전달 단위에 함께 넣는다.',
      '| Instance [Spot application queue]',
      'Instance intent가 없는 Missing Spot message가 Spot 생성 정보를 만들지 않는다.',
    ],
  },
  {
    path: 'framework/doc/framework/common/spec/16-spot-address-messaging.ko.md',
    required: [
      '`SpotHandle`, 별도 resolver handle과 `InstanceSpotAddress`는 제공하지 않는다.',
      '## 3. User Spot Create와 GetOrCreate',
      '## 4. Direct message로 Instance Spot 생성을 허용하는 방법',
      'Spot manager가 Instance Spot create·get-or-create를 제공하지 않는다.',
      'Instance intent가 없는 Missing Spot message가 creation intent를 만들지 않는다.',
      '선택한 Mesh의 distinct Instance type이 하나면 type을 자동 선택하고 여러 개면 type 명시를 요구한다.',
      'Cold activation source가 owner claim을 만들지 않고 최초 message를 포함한 activation envelope를 target에',
      '생성 권한을 얻은 target만 자신을 owner로 기록하고 factory를 실행한다.',
      'Spot manager의 public `Close`는 User Spot의 exact `SpotRef`를 받는다.',
    ],
  },
  {
    path: 'framework/doc/framework/common/spec/15-spot-actor.ko.md',
    required: [
      'Spot의 terminal lifecycle callback은 `OnClosing(ClosingContext)`이다.',
      '| 0 | `ExplicitClose` |',
      '| 1 | `HostShutdown` |',
      '| 2 | `RelocationOut` |',
      'Actor별 closing callback을 제공하지 않는다.',
      'Instance Spot은 source가 first-message activation envelope를 후보 target에 먼저 제출하고 target activation registry가 `Reserve`를 호출한다.',
      'Spot closing만을 위한 별도 Framework cancellation 타입을 만들지는 않는다.',
    ],
  },
  {
    path: 'framework/doc/framework/common/spec/21-location-runtime.ko.md',
    required: [
      'Actor와 User Spot은 Manager의 `Create` 또는 `GetOrCreate`로 만든다.',
      'Framework가 소문자 표준 UUID v4 문자열을 SpotId로 발급한다.',
      'Instance Spot은 별도 생성 API를 사용하지 않는다.',
      'Instance Spot 요청임을 표시했고 Spot이 없을 때만, message를 받은 target node가 Spot을 만든다.',
      '사용 가능한 Instance Spot type이 하나면 선택한다. 0개면 `NotFound`, 둘 이상이면 `InvalidOperation`이다.',
      'Source는 owner나 generation을 미리 만들지 않는다.',
      '동시에 여러 target이 시도해도 성공한 하나만 factory를 실행한다.',
      'Record가 없을 때만 최초 message를 Relocation Store에 저장하고 `Creating` record와 수용 공간을 함께 확보한다.',
    ],
  },
];

const projections = {
  dotnet: [
    'public readonly record struct SpotRef(',
    'string SpotId, ulong ObjectGeneration, string MeshName, RoutingId NodeRid',
    'public interface IZLinkInstanceSpot',
    'public interface IZLinkInstanceSpotHandlerRegistry',
    'IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message);',
    'IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request);',
    'public interface IZLinkSpotSendCall : IZLinkMetadataCall<IZLinkSpotSendCall>',
    'IZLinkSpotSendCall InstanceSpot();',
    'IZLinkSpotSendCall InstanceSpot(string instanceSpotType);',
    'public interface IZLinkSpotRequestCall : IZLinkMetadataCall<IZLinkSpotRequestCall>',
    'IZLinkSpotRequestCall InstanceSpot();',
    'public interface IZLinkSpotManager',
    'IZLinkSpotCreateCall Create(string spotType);',
    'IZLinkSpotGetOrCreateCall GetOrCreate( string spotId, string spotType);',
    '`IZLinkSpotManager`는 User Spot의 명시적 create·get-or-create, resolve와 exact close만 제공한다.',
    'ValueTask<bool> CloseAsync( SpotRef spot,',
    'IZLinkInstanceSpotContext.CloseAsync()',
    'public enum ZLinkSpotCloseReason',
    'ExplicitClose = 0, HostShutdown = 1, RelocationOut = 2, IdleEvicted = 3',
    'public readonly record struct ZLinkSpotClosingContext( ZLinkSpotCloseReason Reason, DateTimeOffset Deadline);',
    'ValueTask OnClosingAsync( ZLinkSpotClosingContext context, CancellationToken cleanupCancellationToken)',
    'Source는 owner claim이나 수용 공간을 미리 확보하지 않는다.',
    'Target에 같은 Spot의 local instance가 없을 때만 자신을 owner로 하는 `Creating` record와 수용 공간을 함께 확보한다.',
  ],
  cpp: [
    'using spot_id_t = std::string;',
    'class spot_ref_t final',
    'std::uint64_t object_generation() const noexcept;',
    'class instance_spot_t {',
    'class instance_spot_handler_registry_t {',
    'spot_send_call_t send_to_spot(spot_id_t target, TCommand command);',
    'spot_request_call_t request_to_spot( spot_id_t target, TRequest request);',
    '`instance_spot()`은 [stable type]',
    '`instance_spot(stable_type)`은 stable type을 명시한다.',
    'distinct Instance Spot type이 0개이면 `not_found`, 둘 이상이면 `invalid_operation`이다.',
    'class spot_manager_t {',
    'virtual spot_create_call_t create(std::string stable_type) = 0;',
    'virtual spot_create_call_t get_or_create( spot_id_t spot_id, std::string stable_type) = 0;',
    '`spot_manager_t`는 User Spot만 생성한다.',
    'virtual task_t<bool> close(spot_ref_t spot) = 0;',
    '`instance_spot_context_t::close()`가 context에 보관한 exact current SpotRef로 local close를 수행한다.',
    'enum class spot_close_reason_t {',
    '    explicit_close = 0,',
    '    host_shutdown = 1,',
    '    relocation_out = 2,',
    '    idle_evicted = 3',
    '};',
    'struct spot_closing_context_t final',
    'std::chrono::system_clock::time_point deadline;',
    'const spot_closing_context_t &context, std::stop_token cleanup_cancellation',
    'Source는 creation reservation을 만들지 않는다.',
    '같은 Spot의 local instance가 없을 때만 Target이 자신을 owner로 등록할 reservation을 요청한다.',
  ],
  java: [
    'public record SpotRef(',
    'String spotId, long objectGeneration, String meshName, RoutingId nodeRid',
    'public interface ZLinkInstanceSpot',
    'public interface ZLinkInstanceSpotHandlerRegistry',
    'public interface ZLinkSpotSendCall extends ZLinkSendCall',
    'ZLinkSpotSendCall instanceSpot();',
    'ZLinkSpotSendCall instanceSpot(String stableType);',
    'public interface ZLinkSpotRequestCall extends ZLinkRequestCall',
    'ZLinkSpotRequestCall instanceSpot();',
    'ZLinkSpotCreateCall create(String spotType);',
    'ZLinkSpotGetOrCreateCall getOrCreate(String spotId, String spotType);',
    'Spot manager는 User Spot 전용이다.',
    'close(systems.zlink.framework.spots.SpotRef);',
    'ZLinkSpotSendCall sendToSpot(java.lang.String, java.lang.Object);',
    'ZLinkSpotRequestCall requestToSpot(java.lang.String, java.lang.Object);',
    'serving 가능한 distinct Instance type이 정확히 하나일 때만 그 type을 사용한다.',
    '`ZLinkInstanceSpotContext.close()`',
    'public enum ZLinkSpotCloseReason',
    'EXPLICIT_CLOSE(0), HOST_SHUTDOWN(1), RELOCATION_OUT(2), IDLE_EVICTED(3);',
    'public record ZLinkSpotClosingContext( ZLinkSpotCloseReason reason, Instant deadline)',
    'onClosing(systems.zlink.framework.spots.ZLinkSpotClosingContext);',
    'Java Spot closing callback에는 별도 Framework cancellation 인자를 추가하지 않는다.',
    'Instance Spot은 source에서 reservation을 만들지 않고 다음 순서로 처리한다.',
    'Instance가 없을 때만 target이 자신을 owner로 하는 `CREATING` authority와 reserved capacity를 예약한다.',
  ],
  kotlin: [
    'SpotId는 UTF-8 encoded 크기 1..255 bytes의 `String`이며',
    '`SpotRef(spotId, objectGeneration, meshName, nodeRid)`는 exact incarnation을 close할 때만',
    '`ZLinkSpotManager.create(spotType)`은 User Spot ID를 생성하고,',
    '`getOrCreate(spotId, spotType)`은 caller가 정한 User',
    'Manager는 Instance Spot create/get-or-create를 제공하지 않는다.',
    'Spot send/request는 global SpotId만 address로 받고 Kotlin 전용 Spot call wrapper를 반환한다.',
    '`instanceSpot()`이나 `instanceSpot(stableType)`을 호출한 call만 Missing Instance Spot의 cold activation intent를 만든다.',
    'serving Instance type이 distinct value 하나일 때만 그 type을 자동 선택한다.',
    '): ZLinkKotlinSpotSendCall',
    '): ZLinkKotlinSpotRequestCall<TReply>',
    'Kotlin은 address DTO, process-local handle, resolver와 unbounded directory를 제공하지 않는다.',
    '`close(SpotRef)`는 Missing이면 `false`, generation 불일치는 `InvalidOperation`',
    '`ZLinkInstanceSpotContext.close()`를 그대로 사용한다.',
    '`ZLinkSpotCloseReason`을 사용하며 값은 `EXPLICIT_CLOSE=0`, `HOST_SHUTDOWN=1`, `RELOCATION_OUT=2`, `IDLE_EVICTED=3`이다.',
    'context: ZLinkSpotClosingContext,',
    'onClosing(systems.zlink.framework.spots.ZLinkSpotClosingContext);',
    '별도 Framework cancellation 타입을 사용하지 않는다.',
    'Source는 placement reservation을 만들지 않는다.',
    'Instance가 local에 없을 때만 target이 자신을 owner로 예약한다.',
  ],
  node: [
    'export type SpotId = string;',
    'export interface SpotRef { readonly spotId: SpotId; readonly objectGeneration: bigint; readonly meshName: string; readonly nodeRid: RoutingId;',
    'export interface ZLinkInstanceSpot {',
    'export interface ZLinkInstanceSpotHandlerRegistry {',
    'sendToSpot(spotId: SpotId, message: unknown): ZLinkSpotSendCall;',
    'requestToSpot(spotId: SpotId, request: unknown): ZLinkSpotRequestCall;',
    'export interface ZLinkSpotSendCall {',
    'instanceSpot(): this;',
    'instanceSpot(instanceSpotType: string): this;',
    'export interface ZLinkSpotRequestCall {',
    'export interface ZLinkSpotManager {',
    'create(spotType: string): ZLinkSpotCreateCall;',
    'getOrCreate( spotId: SpotId, spotType: string): ZLinkSpotGetOrCreateCall;',
    'Instance Spot에는 manager create·get-or-create를 제공하지 않는다.',
    '`instanceSpot()`은 선택한 Mesh의 serving descriptor에 distinct Instance type이 하나일 때',
    'close(spot: SpotRef, signal?: AbortSignal): Promise<boolean>;',
    'export interface ZLinkInstanceSpotContext',
    'close(signal?: AbortSignal): Promise<boolean>;',
    'export declare enum ZLinkSpotCloseReason {',
    '    ExplicitClose = 0,',
    '    HostShutdown = 1,',
    '    RelocationOut = 2,',
    '    IdleEvicted = 3',
    '}',
    'export interface ZLinkSpotClosingContext { readonly reason: ZLinkSpotCloseReason; readonly deadline: Date;',
    'context: ZLinkSpotClosingContext, cleanupSignal: AbortSignal): Promise<void>;',
    'Actor별 closing callback은 제공하지 않는다.',
    'Source는 Store reservation을 만들지 않는다.',
    'Instance가 없을 때만 target이 자신을 owner로 하는 Creating row와 reserved capacity를 예약한다.',
  ],
};

const forbiddenRules = [
  {
    label: 'legacy Instance Spot address',
    pattern: /\b(?:InstanceSpotAddress|instance_spot_address_t)\b/u,
    sample: 'public interface InstanceSpotAddress {}',
  },
  {
    label: 'process-local Spot handle',
    pattern: /\b(?:SpotHandle|ZLinkSpotHandle|IZLinkSpotHandle|spot_handle_t)\b/u,
    sample: 'class spot_handle_t {};',
  },
  {
    label: 'public Spot resolver',
    pattern: /\b(?:ZLinkSpotResolver|IZLinkSpotResolver|SpotResolver|spot_resolver_t|resolveSpot|resolve_spot)\b/u,
    sample: 'interface ZLinkSpotResolver {}',
  },
  {
    label: 'local-only Spot lifecycle',
    pattern: /\b(?:CreateLocalSpot|GetOrCreateLocalSpot|createLocalSpot|getOrCreateLocalSpot|create_local_spot|get_or_create_local_spot)\b/u,
    sample: 'createLocalSpot(type);',
  },
  {
    label: 'first-message creation switch',
    pattern: /\b(?:CreateIfMissing|createIfMissing|create_if_missing)\b/u,
    sample: 'sendToSpot(id, message, createIfMissing);',
  },
  {
    label: 'legacy Instance-specific lifecycle operation',
    pattern: /\b(?:CreateInstanceSpot|GetOrCreateInstanceSpot|createInstanceSpot|getOrCreateInstanceSpot|create_instance_spot|get_or_create_instance_spot)\b/u,
    sample: 'createInstanceSpot(type);',
  },
  {
    label: 'legacy kind-selecting Spot manager',
    pattern: /\b(?:ZLinkCreatableSpotKind|IZLinkCreatableSpotKind)\b/u,
    sample: 'create(kind: ZLinkCreatableSpotKind, spotType: string);',
  },
  {
    label: 'target-selecting Spot create',
    pattern: /(?:IZLinkSpotCreateCall\s+Create|ZLinkSpotCreateCall\s+create|spot_create_call_t\s+create)\s*\([^)]*\b(?:NodeRid|nodeRid|node_rid|targetNode|target_node|endpoint)\b[^)]*\)/su,
    sample: 'ZLinkSpotCreateCall create(NodeRid targetNode, String type);',
  },
  {
    label: 'Java-specific Spot closing cancellation token',
    pattern: /\bZLinkSpotClosingCancellation\b/u,
    sample: 'interface ZLinkSpotClosingCancellation {}',
  },
  {
    label: 'removed placement profile selector',
    pattern: /\b(?:PlacementProfile|placementProfile|placement_profile|ZLinkPlacementProfile)\b/u,
    sample: 'interface ZLinkPlacementProfile {}',
  },
  {
    label: 'removed affinity key selector',
    pattern: /\b(?:AffinityKey|affinityKey|affinity_key|ZLinkAffinityKey)\b/u,
    sample: 'interface ZLinkAffinityKey {}',
  },
];

const contracts = new Map(languages.map(language => [
  language,
  readExactContract(root, language, tags[language]),
]));

const missingProjectionFailures = (language, source) => projections[language]
  .filter(fragment => !normalized(source).includes(normalized(fragment)))
  .map(fragment => `${language} exact interface is missing "${normalized(fragment)}"`);
const forbiddenContractFailures = (language, code) => forbiddenRules
  .filter(rule => rule.pattern.test(code))
  .map(rule => `${language} exact interface exposes ${rule.label}`);

const failures = [];
for (const fixture of formalFixtures) {
  const source = normalized(read(fixture.path));
  for (const fragment of fixture.required) {
    if (!source.includes(normalized(fragment))) {
      failures.push(`formal contract is missing "${normalized(fragment)}": ${fixture.path}`);
    }
  }
}

for (const language of languages) {
  const contract = contracts.get(language);
  failures.push(...missingProjectionFailures(language, contract.source));
  failures.push(...forbiddenContractFailures(language, contract.code));
}

const actorFreeRules = [
  ['dotnet', /interface\s+IZLinkInstanceSpot\s*:\s*IZLinkSpot\b/su],
  ['cpp', /class\s+instance_spot_t\s*:\s*public\s+spot_t\b/su],
  ['java', /interface\s+ZLinkInstanceSpot\s+extends\s+ZLinkSpot\b/su],
  ['node', /interface\s+ZLinkInstanceSpot\s+extends\s+ZLinkSpot\b/su],
];
for (const [language, pattern] of actorFreeRules) {
  if (pattern.test(contracts.get(language).code)) {
    failures.push(`${language} Instance Spot lifecycle inherits the actor-capable Spot interface`);
  }
}

if (failures.length > 0) {
  process.stderr.write(`${failures.map(message => `FAIL: ${message}`).join('\n')}\n`);
  process.exit(1);
}

let negativeMutations = 0;
if (mode === '--self-test') {
  for (const rule of forbiddenRules) {
    const rejected = forbiddenContractFailures('negative', rule.sample)
      .some(failure => failure.includes(rule.label));
    if (!rejected) {
      throw new Error(`negative self-test did not reject ${rule.label}`);
    }
    negativeMutations += 1;
  }
  for (const language of languages) {
    const fragment = normalized(projections[language][0]);
    const source = normalized(contracts.get(language).source);
    const mutated = source.split(fragment).join('');
    if (!missingProjectionFailures(language, mutated)
      .some(failure => failure.includes(fragment))) {
      throw new Error(`negative self-test did not reject missing ${language} required projection`);
    }
    negativeMutations += 1;
  }
}

process.stdout.write(
  `INSTANCE SPOT CONTRACTS CLEAN languages=${languages.length}`
  + ` formal_documents=${formalFixtures.length}`
  + ` required_fragments=${Object.values(projections).flat().length}`
  + ` forbidden_rules=${forbiddenRules.length}`
  + ` negative_mutations=${negativeMutations}\n`);
NODE
