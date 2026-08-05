#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:---contract}"
case "$mode" in
  --contract|--implementation) ;;
  *) echo "usage: $0 [--contract|--implementation]" >&2; exit 2 ;;
esac

node - "$repo_root" "$mode" <<'NODE'
'use strict';

const fs = require('fs');
const path = require('path');
const root = process.argv[2];
const mode = process.argv[3];
const { readExactContract } = require(
  path.join(root, 'scripts/lib/framework-contract-documents.cjs'));
const languages = ['dotnet', 'cpp', 'java', 'kotlin', 'node'];
const tags = {
  dotnet: ['csharp'], cpp: ['cpp'], java: ['java'],
  kotlin: ['kotlin', 'java'], node: ['ts', 'typescript'],
};
const failures = [];
const fail = message => failures.push(message);

function read(relative) {
  const absolute = path.join(root, relative);
  if (!fs.existsSync(absolute)) {
    fail(`missing contract file: ${relative}`);
    return '';
  }
  return fs.readFileSync(absolute, 'utf8');
}

if (mode === '--contract') {
  const asyncPolicy = read('framework/doc/framework/common/spec/05-async-execution-policy.ko.md');
  const frameworkApi = read('framework/doc/framework/common/spec/06-framework-api.ko.md');
  const spotMessaging = read('framework/doc/framework/common/spec/12-spot-messaging.ko.md');
  const scenario = read('framework/doc/framework/common/e2e/config-13-submit-admission.ko.md');
  for (const [source, owner, fragments] of [
    [asyncPolicy, 'async policy', [
      '동기 `TrySubmit` 계열을 제공하지 않는다',
      '반환 데이터 없이 완료',
      '`DeadlineExceeded`',
      '`ShuttingDown`']],
    [frameworkApi, 'Framework API', [
      'one-way send·publish는 결과값 없이 정상 완료',
      'Target별 수락·실패 결과는 public publish 결과로 반환하거나 publish 전용 monitoring 값으로 집계하지',
      '`ShuttingDown`']],
    [spotMessaging, 'Spot messaging', [
      'Publish 완료는 handler 실행 결과가 아니라 local outbound admission',
      'monitoring snapshot, metric 또는 runtime event로 제공하지 않는다']],
    [scenario, 'Config 13', [
      '원격 handler 실행 완료를 뜻하지 않는다',
      'timeout 예외',
      'Shutdown']]
  ]) {
    for (const fragment of fragments) {
      if (!source.includes(fragment)) fail(`${owner} is missing submit semantic: ${fragment}`);
    }
  }

  const contracts = new Map(languages.map(language => [
    language, readExactContract(root, language, tags[language]),
  ]));
  const submitPatterns = {
    dotnet: /ValueTask\s+Async\s*\(/,
    cpp: /task_t<void>\s+submit\s*\(\s*\)/,
    java: /CompletionStage<(?:java\.lang\.)?Void>\s+submit\s*\(/,
    kotlin: /suspend\s+fun\s+await\s*\(\s*\)(?:\s*:\s*Unit)?/,
    node: /submit\([^)]*\): Promise<void>/,
  };
  for (const language of languages) {
    const contract = contracts.get(language);
    if (!submitPatterns[language].test(contract.source)) {
      fail(`${language} async-only submit projection is missing`);
    }
    if (/\b(?:TrySubmit|trySubmit|try_submit)\s*(?:<[^>]*>)?\s*\(/.test(contract.code)) {
      fail(`${language} exact public declarations expose a synchronous TrySubmit terminator`);
    }
    if (/\b(?:ZLinkSubmitStatus|ZLinkSubmitResult|ZLinkPublishResult|ZLinkLogicalMulticastDetail|submit_status_t|submit_result_t|publish_result_t|logical_multicast_detail_t)\b/.test(contract.code)) {
      fail(`${language} exact public declarations expose a removed one-way result type`);
    }
    if (language === 'dotnet' && /\bSubmitAsync\s*\(/.test(contract.code)) {
      fail('dotnet exact public declarations retain SubmitAsync');
    }
    if (/\b(?:ActorRelocationTimeout|actorRelocationTimeout|actor_relocation_timeout)\b/.test(contract.code)) {
      fail(`${language} exact public declarations expose an Actor-specific relocation timeout`);
    }
  }

  const messageFollowDurationFragments = {
    dotnet: 'MessageFollowDuration',
    cpp: 'message_follow_duration',
    java: 'messageFollowDuration',
    node: 'messageFollowDurationMs',
  };
  for (const [language, fragment] of Object.entries(messageFollowDurationFragments)) {
    if (!contracts.get(language).source.includes(fragment)) {
      fail(`${language} host-wide Message Follow duration is missing`);
    }
  }
  const removedMessageFollowNames =
    /\b(?:RelocationForwardingWindow|relocationForwardingWindow(?:Ms)?|relocation_forwarding_window|ActorTransferForwardWindow|actorTransferForwardWindow(?:Ms)?|actor_transfer_forward_window)\b/;
  for (const [language, contract] of contracts) {
    if (removedMessageFollowNames.test(contract.code)) {
      fail(`${language} exact public declarations retain a removed forwarding-window name`);
    }
  }

  const directRouteContextFragments = {
    dotnet: ['ZLinkRouteMessageContext', 'string? MeshName', 'RoutingId SourceNodeRid'],
    cpp: ['route_message_context_t', 'std::string mesh_name', 'source_node_rid'],
    java: ['ZLinkRouteMessageContext', 'meshName()', 'sourceNodeRid()'],
    node: ['ZLinkRouteMessageContext', 'readonly meshName: string', 'readonly sourceNodeRid: RoutingId'],
  };
  for (const [language, fragments] of Object.entries(directRouteContextFragments)) {
    const source = contracts.get(language).source;
    for (const fragment of fragments) {
      if (!source.includes(fragment)) {
        fail(`${language} direct-node handler context is missing ${fragment}`);
      }
    }
  }

  const removedMulticastMonitoring =
    /\b(?:ZLinkLogicalMulticastSnapshot|logical_multicast_snapshot_t|RemoteSnapshotCount|RemoteAdmittedCount|RemoteDroppedCount|RemoteUnreachableCount|LocalSnapshotCount|LocalAdmittedCount|LocalDroppedCount|remoteSnapshotCount|remoteAdmittedCount|remoteDroppedCount|remoteUnreachableCount|localSnapshotCount|localAdmittedCount|localDroppedCount|remote_snapshot_count|remote_admitted_count|remote_dropped_count|remote_unreachable_count|local_snapshot_count|local_admitted_count|local_dropped_count)\b/;
  for (const [language, contract] of contracts) {
    if (removedMulticastMonitoring.test(contract.code)) {
      fail(`${language} retains removed Logical Multicast monitoring declarations`);
    }
  }

  const dotnetMulticast = contracts.get('dotnet').code;
  if (!/IZLinkPublishCall\s+Publish<TEvent>\(\s*string channelName,\s*string topic,\s*TEvent message\)/s.test(dotnetMulticast)) {
    fail('dotnet Logical Multicast ChannelName-and-topic projection is missing');
  }
  if (/\bPublishSpot\s*</.test(dotnetMulticast)
      || /Publish<TEvent>\(\s*string meshName,\s*string channelName/s.test(dotnetMulticast)
      || /IZLinkPublishCall\s+Publish<TEvent>\(\s*string topic,\s*TEvent message\)/s.test(dotnetMulticast)) {
    fail('dotnet Logical Multicast retains an alias, MeshName overload, or implicit ChannelName overload');
  }

  const cppMulticast = contracts.get('cpp').code;
  if (!/publish_call_t\s+publish\(\s*std::string channel_name,\s*std::string topic,/s.test(cppMulticast)
      || !/add_subscribe\(\s*std::string channel_name,\s*std::string topic\)/s.test(cppMulticast)) {
    fail('cpp Logical Multicast and subscription must both name ChannelName and topic');
  }
  if (/send_call_t\s+publish\(\s*std::string topic,/s.test(cppMulticast)) {
    fail('cpp Spot context retains an implicit ChannelName publish overload');
  }

  // Classic fanout keeps both public meanings: the convenience overload derives
  // the topic from the packet name, while the second overload accepts a topic.
  const fanoutChecks = {
    dotnet: [
      /Publish<TEvent>\(\s*string channelName,\s*TEvent message\)/s,
      /Publish<TEvent>\(\s*string channelName,\s*string topic,\s*TEvent message\)/s,
    ],
    cpp: [
      /publish\(std::string channel_name,\s*(?:const )?TEvent(?:\s*&)?\s*event\)/s,
      /publish\(std::string channel_name,\s*std::string topic,\s*(?:const )?TEvent(?:\s*&)?\s*event\)/s,
    ],
    java: [
      /publish\(\s*String channelName,\s*Object event\)/s,
      /publish\(\s*String channelName,\s*String topic,\s*Object event\)/s,
    ],
    kotlin: [
      /fun publish\(\s*channelName: String,\s*event: Any/s,
      /fun publish\(\s*channelName: String,\s*topic: String,\s*event: Any/s,
    ],
    node: [
      /publish\(channelName: string, event: unknown\): ZLinkFanoutPublishCall/,
      /publish\(channelName: string, topic: string, event: unknown\): ZLinkFanoutPublishCall/,
    ],
  };
  for (const [language, patterns] of Object.entries(fanoutChecks)) {
    const source = contracts.get(language).source;
    if (!patterns[0].test(source)) fail(`${language} classic fanout packet-name-derived overload is missing`);
    if (!patterns[1].test(source)) fail(`${language} classic fanout explicit-topic overload is missing`);
  }

  for (const fragment of [
    '즉시 수락',
    'capacity 대기',
    'timeout 예외',
    'cancellation',
    'Shutdown',
    'target',
    'route',
    'target이 0개',
    'partial',
    'monitoring',
  ]) {
    if (!scenario.toLowerCase().includes(fragment.toLowerCase())) {
      fail(`Config 13 is missing one-way scenario coverage: ${fragment}`);
    }
  }

  if (failures.length) {
    process.stderr.write(`${failures.map(message => `[submit-api] FAIL: ${message}`).join('\n')}\n`);
    process.exit(1);
  }
  process.stdout.write(`[submit-api] contract CLEAN languages=${languages.length} result_free=1 fanout_overloads=10\n`);
  process.exit(0);
}

function walk(directory, extensions, output = []) {
  if (!fs.existsSync(directory)) return output;
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) walk(absolute, extensions, output);
    else if (extensions.some(extension => entry.name.endsWith(extension))) output.push(absolute);
  }
  return output;
}

// Implementation mode is intentionally separate from the document gate. It
// scans only installed/public source roots; internal non-blocking primitives
// may keep their implementation-specific names.
const publicRoots = [
  ['dotnet', 'framework/languages/dotnet/src/Zlink.Framework/Contracts', ['.cs']],
  ['cpp', 'framework/languages/cpp/framework/include', ['.h', '.hpp']],
  ['java', 'framework/languages/java/zlink-framework-core/src/main/java', ['.java']],
  ['kotlin', 'framework/languages/java/zlink-framework-kotlin/src/main/kotlin', ['.kt']],
  ['node', 'framework/languages/node/packages/framework/src/contracts', ['.ts']],
];
let scanned = 0;
for (const [language, relative, extensions] of publicRoots) {
  const absolute = path.join(root, relative);
  if (!fs.existsSync(absolute)) {
    fail(`${language} public source root is missing: ${relative}`);
    continue;
  }
  for (const file of walk(absolute, extensions)) {
    scanned += 1;
    const source = fs.readFileSync(file, 'utf8');
    const exposesRemovedTerminator = language === 'java' || language === 'kotlin'
      ? /^\s*public\s+[^\n;{}]*\b(?:TrySubmit|trySubmit|try_submit)\s*(?:<[^>]*>)?\s*\(/m.test(source)
      : /\b(?:TrySubmit|trySubmit|try_submit)\s*(?:<[^>]*>)?\s*\(/.test(source);
    if (exposesRemovedTerminator) {
      fail(`${language} public source retains TrySubmit: ${path.relative(root, file)}`);
    }
    if (/\b(?:ZLinkSubmitStatus|ZLinkSubmitResult|ZLinkPublishResult|ZLinkLogicalMulticastDetail|submit_status_t|publish_result_t|logical_multicast_detail_t)\b/.test(source)) {
      fail(`${language} public source retains a removed one-way result type: ${path.relative(root, file)}`);
    }
    if (language === 'dotnet' && /\bSubmitAsync\s*\(/.test(source)) {
      fail(`dotnet public source retains SubmitAsync: ${path.relative(root, file)}`);
    }
  }
}

const kotlinOneWayWrappers = read(
  'framework/languages/java/zlink-framework-kotlin/src/main/kotlin/'
  + 'systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt');
for (const fragment of [
  'interface ZLinkKotlinMessageSendCall',
  'interface ZLinkKotlinSpotSendCall',
  'interface ZLinkKotlinSpotRequestCall',
  'suspend fun await()',
  'fun ZLinkKotlinRouteClient.sendToSpot(',
  'fun <TReply : Any> ZLinkKotlinRouteClient.requestToSpot(',
]) {
  if (!kotlinOneWayWrappers.includes(fragment)) {
    fail(`kotlin one-way wrapper is missing: ${fragment}`);
  }
}

for (const relative of [
  'framework/languages/java/zlink-framework-core/src/main/java/'
    + 'systems/zlink/framework/runtime/messaging/ZLinkOneWayAdmission.java',
  'framework/languages/java/zlink-framework-core/src/main/java/'
    + 'systems/zlink/framework/runtime/messaging/ZLinkOneWayAdmissionStatus.java',
]) {
  const source = read(relative);
  if (/^\s*public\s+(?:final\s+)?(?:class|enum|interface)\s+ZLinkOneWay/m.test(source)) {
    fail(`java internal one-way helper is exposed as public: ${relative}`);
  }
}

const javaBackendObject = read(
  'framework/languages/java/zlink-framework-core/src/main/java/'
  + 'systems/zlink/framework/runtime/backend/ZLinkBackendObject.java');
if (/\b(?:ONE_WAY_|submitOneWay|beginOneWay|adaptOneWay|oneWayStatus)\b/.test(
  javaBackendObject)) {
  fail('java public backend interface exposes internal one-way admission helpers');
}
if (/\b(?:admissionSource|setAdmissionReadyHandler|setAdmissionShutdownHandler|admissionTimeout|admissionPendingCapacity)\b/
  .test(javaBackendObject)) {
  fail('java public backend interface exposes admission runtime wiring');
}

if (/class StageMessageSendCall[\s\S]*?override fun metadata\([^)]*\)[\s\S]*?=\s*this/
  .test(kotlinOneWayWrappers)) {
  fail('kotlin stage-backed one-way wrapper silently discards metadata');
}
if (/metadataSupported\s*=\s*false/.test(kotlinOneWayWrappers)) {
  fail('kotlin canonical one-way wrapper exposes metadata but rejects it at runtime');
}

const packageNamingSources = {
  dotnetHttp: read(
    'framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs'),
  dotnetConnector: [
    'framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/'
      + 'Runtime/Calls/ZlinkStreamSendBuilder.cs',
    'framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/'
      + 'Runtime/Calls/ZlinkStreamExpectNoneBuilder.cs',
  ].map(read).join('\n'),
  javaHttp: read(
    'framework/languages/java/zlink-http-client/src/main/java/'
      + 'systems/zlink/httpclient/ZLinkHttpRequestBuilder.java'),
  javaConnector: read(
    'framework/languages/java/zlink-stream-connector/src/main/java/'
      + 'systems/zlink/stream/connector/ZLinkStreamSendCall.java'),
  kotlinHttp: read(
    'framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/'
      + 'systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt'),
  nodeHttp: read(
    'framework/languages/node/packages/http-client/src/request-builder.ts'),
  nodeConnector: read(
    'framework/languages/node/packages/stream-connector/src/Contracts/Calls/'
      + 'ZlinkStreamCalls.ts'),
  cppHttp: read(
    'framework/languages/cpp/http-client/include/zlink/http_client/'
      + 'contracts/client.hpp'),
  cppConnector: read(
    'framework/languages/cpp/connector/core/include/zlink/stream_connector/'
      + 'contracts/calls/zlink_stream_calls.hpp'),
};
if (/\bSubmitAsync\s*\(/.test(
  packageNamingSources.dotnetHttp + packageNamingSources.dotnetConnector)) {
  fail('dotnet HTTP Client or Stream Connector retains SubmitAsync');
}
if (!/\bsubmitRaw\s*\(/.test(packageNamingSources.javaHttp)
    || !/CompletionStage<Void>\s+submit\s*\(/.test(
      packageNamingSources.javaConnector)) {
  fail('java HTTP Client or Stream Connector naming is not canonical');
}
if (!/suspend\s+fun[\s\S]*\bawait\s*\(/.test(packageNamingSources.kotlinHttp)) {
  fail('kotlin HTTP Client coroutine wrapper is missing await');
}
if (/\basyncRaw\s*\(/.test(packageNamingSources.nodeHttp)
    || !/\bsubmitRaw\s*\(/.test(packageNamingSources.nodeHttp)
    || !/Promise<void>/.test(packageNamingSources.nodeConnector)) {
  fail('node HTTP Client or Stream Connector naming is not canonical');
}
if (!/\bsubmit\s*\(/.test(packageNamingSources.cppHttp)
    || !/\bsubmit\s*\(/.test(packageNamingSources.cppConnector)) {
  fail('cpp HTTP Client or Stream Connector naming is not canonical');
}

const cppSubmitRuntime = read(
  'framework/languages/cpp/framework/src/runtime/messaging/'
  + 'async_submit_runtime.cpp');
const cppMulticastExecutor = cppSubmitRuntime.match(
  /class logical_multicast_executor_t[\s\S]*?\n};/);
if (!cppMulticastExecutor) {
  fail('cpp Logical Multicast executor implementation is missing');
} else if (/\b_pending\b/.test(cppMulticastExecutor[0])) {
  fail('cpp Logical Multicast executor retains a pending payload queue');
}
const cppGeneralExecutor = cppSubmitRuntime.match(
  /class async_submit_runtime_t[\s\S]*?\n};/);
if (!cppGeneralExecutor) {
  fail('cpp general one-way executor implementation is missing');
} else if (/\b_waiting\b/.test(cppGeneralExecutor[0])) {
  fail('cpp general one-way executor retains an unbounded overflow queue');
}

if (failures.length) {
  process.stderr.write(`${failures.map(message => `[submit-api] IMPLEMENTATION GAP: ${message}`).join('\n')}\n`);
  process.exit(1);
}
process.stdout.write(`[submit-api] implementation CLEAN public_files=${scanned}\n`);
NODE
