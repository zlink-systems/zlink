#!/usr/bin/env node

// SPDX-License-Identifier: MPL-2.0

import {spawnSync} from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, '..');
const inventoryRepositoryPath =
  'framework/doc/contract-inventory/route-mesh-v11-core-service-migration-inventory.json';
const inventoryPath = process.env.ZLINK_V11_MIGRATION_INVENTORY_PATH
  ? path.resolve(process.env.ZLINK_V11_MIGRATION_INVENTORY_PATH)
  : path.join(repositoryRoot, inventoryRepositoryPath);

// The reviewed inventory is also the immutable historical source for the Core
// service surface after M3 removes its headers and exports.  Only these two
// sections participate in the digest, so later reviewed classifications for
// bindings and Framework files do not rewrite Core 10.x history.
const reviewedCoreBaselineSha256 =
  '96fe0a924d106106ed9f323bc13f1300668a160052b863494fc8eba7749eb1ec';
const reviewedCoreRemovalFilesRevision =
  '1de8f43917d7c8d3d0f26dadf97c9f83ede79228';
const reviewedCoreRemovalFilesSha256 =
  'f8a9d9c576d9d93596db31cb3251875f0429e2cf6829a749f399143ef3baf3f0';

const ledgerPath =
  'framework/doc/plan/for-interals/framework-internals-implementation-gaps.ko.md';
const formalSpecRoot = 'framework/doc/framework/common/spec';
const formalServerSpecRoot = formalSpecRoot;
const commonInternalsRoot = 'framework/doc/framework/common/internals';
const formalSpecIndex = `${formalSpecRoot}/README.ko.md`;
const commonInternalsIndex = `${commonInternalsRoot}/README.ko.md`;
const targetSpecRoot = formalSpecRoot;
const targetInternalsRoot = commonInternalsRoot;
const retiredPlanSpecRoot = `framework/doc/plan/v11.0/${['target', 'spec'].join('-')}`;
const retiredPlanInternalsRoot = `framework/doc/plan/v11.0/${['target', 'internals'].join('-')}`;

const allowedDispositions = new Set([
  'target-contract',
  'raw-core-prerequisite',
  'superseded',
  'remove',
  'retain-10x-only',
  'out-of-scope-v11-service-migration',
]);

// The disposition classifies the migration meaning of a record. The action
// definition separately states whether that meaning applies to a whole file,
// one public symbol, or only the service fragment inside a mixed file.
const actionDefinitions = Object.freeze({
  'exclude-from-core-11-build-package-and-compatibility-metadata': {scope: 'whole-file', meaning: 'Keep the file outside Core 11 artifacts and compatibility claims.'},
  'migrate-sample-then-remove-service-reference': {scope: 'partial-file', meaning: 'Migrate the service use, then remove only that use from the sample.'},
  'migrate-test-then-remove-service-reference': {scope: 'partial-file', meaning: 'Migrate the covered behavior, then remove only the service-dependent test fragment.'},
  'preserve-meaning-in-target-document-not-file': {scope: 'whole-file', meaning: 'The historical file does not return; its meaning is owned by the target document.'},
  'remove-after-oracle-baseline-sealed': {scope: 'whole-file', meaning: 'Remove the active legacy performance input after its baseline is sealed.'},
  'remove-binding-service-declaration-after-framework-runtime-replacement': {scope: 'symbol', meaning: 'Remove this binding public declaration after the Framework replacement exists.'},
  'remove-binding-service-projection': {scope: 'symbol-reference', meaning: 'Remove this exact Core service symbol reference from the binding.'},
  'remove-binding-service-projection-after-framework-runtime-replacement': {scope: 'whole-file', meaning: 'Remove the binding service projection file after the Framework replacement exists.'},
  'remove-core-service-header-copy-before-core-11-package': {scope: 'partial-file', meaning: 'Remove only the service-header copy operation from the package script.'},
  'remove-framework-binding-service-projection-reference': {scope: 'partial-file', meaning: 'Remove Core or binding service runtime access while retaining the Framework-owned contract and runtime file.'},
  'remove-core-service-only-document-after-target-meaning-is-owned-by-framework': {scope: 'whole-file', meaning: 'Remove the Core-only service document after Framework owns its contract.'},
  'remove-export-from-core-11': {scope: 'symbol', meaning: 'Remove this exact export symbol from Core 11.'},
  'remove-file': {scope: 'whole-file', meaning: 'Remove the complete file at the stated gate.'},
  'remove-service-build-reference': {scope: 'partial-file', meaning: 'Remove only the service build entry from the mixed build file.'},
  'remove-service-projection': {scope: 'partial-file', meaning: 'Remove only the binding service projection from the mixed file.'},
  'remove-service-reference': {scope: 'partial-file', meaning: 'Remove only the classified service reference from the mixed file.'},
  'remove-spot-envelope-from-core-zmp-guide-and-use-framework-service-wire-owner': {scope: 'partial-file', meaning: 'Remove the service envelope section while retaining the Core ZMP guide.'},
  'remove-symbol-from-core-11': {scope: 'symbol', meaning: 'Remove this exact public Core symbol.'},
  'remove-zmp-heartbeat-option-frame-timer-or-test': {scope: 'partial-file', meaning: 'Remove only the ZMP heartbeat option, frame, timer, or test fragment from the mixed file.'},
  'remove-zmp-heartbeat-option-projection': {scope: 'partial-file', meaning: 'Remove only the ZMP heartbeat option projection from the binding file.'},
  'replace-binding-service-projection-with-language-owned-runtime-and-public-raw-binding': {scope: 'partial-file', meaning: 'Replace the service projection use while retaining unrelated Framework code.'},
  'replace-generated-output': {scope: 'whole-file', meaning: 'Regenerate or replace the complete generated artifact.'},
  'replace-package-payload': {scope: 'whole-file', meaning: 'Replace the complete packaged binary payload.'},
  'retain-and-exclude-from-server-framework-service-migration': {scope: 'whole-file', meaning: 'Retain the adjacent component unchanged outside this migration.'},
  'retain-framework-owned-service-wire-input': {scope: 'whole-file', meaning: 'Retain this Framework-owned service wire input.'},
  'retain-framework-owned-service-runtime': {scope: 'whole-file', meaning: 'Retain the Framework-owned service contract value type or runtime implementation; this is not a binding projection.'},
  'retain-generic-scheduler-regression-and-remove-service-specific-assertions': {scope: 'partial-file', meaning: 'Retain the generic test and remove only service assertions.'},
  'retain-or-rename-generic-scheduler-and-remove-service-specific-state': {scope: 'partial-file', meaning: 'Retain the generic scheduler and remove only service state.'},
  'retain-read-only-archive': {scope: 'whole-file', meaning: 'Retain the file only as a read-only Core 10.x archive.'},
  'retain-reviewed-raw-boundary-or-non-service-usage': {scope: 'whole-file', meaning: 'Retain the reviewed raw or unrelated document.'},
  'review-binding-package-tooling-before-package-execution': {scope: 'whole-file', meaning: 'Retain the package tool and review it before package execution.'},
  'review-core-package-tooling-before-package-execution': {scope: 'whole-file', meaning: 'Retain the Core package tool and review it before the Core 11 raw-only package gate.'},
  'rewrite-as-core-11-raw-only-guide-and-remove-service-reference': {scope: 'partial-file', meaning: 'Retain the guide and rewrite only its service-dependent content.'},
  'rewrite-as-core-11-raw-only-internals-and-remove-service-design': {scope: 'partial-file', meaning: 'Retain the internals document and remove only the service design.'},
  'rewrite-formal-contract-as-core-11-raw-only-contract': {scope: 'partial-file', meaning: 'Retain the formal document and replace only its service contract.'},
  'verify-and-remove-service-package-reference': {scope: 'partial-file', meaning: 'Retain the package or CI input and remove only the service reference.'},
});

const coreServiceHeaders = [
  'core/include/zlink/service/actor.h',
  'core/include/zlink/service/common.h',
  'core/include/zlink/service/dispatch.h',
  'core/include/zlink/service/instance_spot_driver.h',
  'core/include/zlink/service/mesh_node.h',
  'core/include/zlink/service/spot.h',
  'core/include/zlink/service/stream_session.h',
];

const coreSupportHeaders = [
  'core/include/zlink/eventing/api.h',
  'core/include/zlink/socket/api.h',
  'core/include/zlink_enum.h',
];

const historicalServiceDocuments = [
  'core/doc/internals/services-internals.ko.md',
  'core/doc/internals/services-internals.md',
  'core/doc/spec/core/service/README.ko.md',
  'core/doc/spec/core/service/README.md',
  'core/doc/spec/core/service/01-mesh-node.ko.md',
  'core/doc/spec/core/service/01-mesh-node.md',
  'core/doc/spec/core/service/02-dispatch.ko.md',
  'core/doc/spec/core/service/02-dispatch.md',
  'core/doc/spec/core/service/03-spot.ko.md',
  'core/doc/spec/core/service/03-spot.md',
  'core/doc/spec/core/service/04-actor.ko.md',
  'core/doc/spec/core/service/04-actor.md',
  'core/doc/spec/core/service/05-stream-session.ko.md',
  'core/doc/spec/core/service/05-stream-session.md',
];

const coreServiceDocumentationReviewGroups = [
  {
    reviewDecision: 'Remove',
    category: 'core-service-only-guide',
    disposition: 'superseded',
    action: 'remove-core-service-only-document-after-target-meaning-is-owned-by-framework',
    removalGate: 'V11-M3-CORE-CLEAN',
    finalGate: 'V11-M9-DOCS',
    targetOwners: [
      `${targetSpecRoot}/README.ko.md`,
      `${targetInternalsRoot}/README.ko.md`,
    ],
    files: [
      'core/doc/guide/07-0-services.ko.md',
      'core/doc/guide/07-0-services.md',
      'core/doc/guide/07-3-spot.ko.md',
      'core/doc/guide/07-3-spot.md',
      'core/doc/guide/07-4-actor.ko.md',
      'core/doc/guide/07-4-actor.md',
    ],
  },
  {
    reviewDecision: 'Rewrite',
    category: 'core-mixed-guide-service-reference',
    disposition: 'superseded',
    action: 'rewrite-as-core-11-raw-only-guide-and-remove-service-reference',
    removalGate: 'V11-M3-CORE-CLEAN',
    finalGate: 'V11-M9-DOCS',
    targetOwners: [
      'core/doc/spec/core/09-runtime-boundary.ko.md',
      `${targetSpecRoot}/README.ko.md`,
      `${targetInternalsRoot}/README.ko.md`,
    ],
    files: [
      'core/doc/README.ko.md',
      'core/doc/guide/README.ko.md',
      'core/doc/guide/01-overview.ko.md',
      'core/doc/guide/01-overview.md',
      'core/doc/guide/02-core-api.ko.md',
      'core/doc/guide/02-core-api.md',
      'core/doc/guide/03-0-socket-patterns.ko.md',
      'core/doc/guide/03-0-socket-patterns.md',
      'core/doc/guide/03-4-router.ko.md',
      'core/doc/guide/03-4-router.md',
      'core/doc/guide/03-5-stream.ko.md',
      'core/doc/guide/03-5-stream.md',
      'core/doc/guide/05-tls-security.ko.md',
      'core/doc/guide/05-tls-security.md',
      'core/doc/guide/06-monitoring.ko.md',
      'core/doc/guide/06-monitoring.md',
      'core/doc/guide/08-routing-id.ko.md',
      'core/doc/guide/08-routing-id.md',
      'core/doc/guide/09-message-api.ko.md',
      'core/doc/guide/09-message-api.md',
      'core/doc/guide/10-performance.ko.md',
      'core/doc/guide/10-performance.md',
      'core/doc/guide/11-thread-safety.ko.md',
      'core/doc/guide/11-thread-safety.md',
      'core/doc/guide/12-socket-options.ko.md',
      'core/doc/guide/12-socket-options.md',
      'core/doc/guide/glossary.ko.md',
      'core/doc/guide/reliability.ko.md',
      'core/doc/guide/scenarios.ko.md',
    ],
  },
  {
    reviewDecision: 'Rewrite',
    category: 'core-zmp-guide-service-wire-reference',
    disposition: 'superseded',
    action: 'remove-spot-envelope-from-core-zmp-guide-and-use-framework-service-wire-owner',
    removalGate: 'V11-M3-CORE-CLEAN',
    finalGate: 'V11-M9-DOCS',
    targetOwners: [
      'core/doc/internals/protocol-zmp.ko.md',
      `${commonInternalsRoot}/service-wire-protocol.ko.md`,
      `${targetSpecRoot}/README.ko.md`,
    ],
    files: ['core/doc/guide/zmp-protocol.ko.md'],
  },
  {
    reviewDecision: 'Rewrite',
    category: 'core-mixed-internals-service-reference',
    disposition: 'superseded',
    action: 'rewrite-as-core-11-raw-only-internals-and-remove-service-design',
    removalGate: 'V11-M3-CORE-CLEAN',
    finalGate: 'V11-M9-DOCS',
    targetOwners: [
      'core/doc/internals/runtime-boundary.ko.md',
      `${targetInternalsRoot}/README.ko.md`,
    ],
    files: [
      'core/doc/internals/architecture.ko.md',
      'core/doc/internals/architecture.md',
      'core/doc/internals/connection-memory.ko.md',
      'core/doc/internals/connection-memory.md',
      'core/doc/internals/design-decisions.ko.md',
      'core/doc/internals/design-decisions.md',
      'core/doc/internals/multipart-atomicity.ko.md',
      'core/doc/internals/socket-option-defaults.ko.md',
      'core/doc/internals/socket-option-defaults.md',
    ],
  },
  {
    reviewDecision: 'Rewrite',
    category: 'core-formal-spec-service-reference',
    disposition: 'superseded',
    action: 'rewrite-formal-contract-as-core-11-raw-only-contract',
    removalGate: 'SPEC-03',
    finalGate: 'V11-M9-DOCS',
    targetOwners: [
      'core/doc/spec/core/09-runtime-boundary.ko.md',
      `${targetSpecRoot}/README.ko.md`,
    ],
    files: [
      'core/doc/spec/core/02-message.ko.md',
      'core/doc/spec/core/02-message.md',
      'core/doc/spec/core/03-errors.ko.md',
      'core/doc/spec/core/03-errors.md',
      'core/doc/spec/core/socket/README.ko.md',
      'core/doc/spec/core/socket/README.md',
      'core/doc/spec/sample/SAMPLE_POLICY.md',
    ],
  },
  {
    reviewDecision: 'Retain',
    category: 'reviewed-raw-core-document',
    disposition: 'raw-core-prerequisite',
    action: 'retain-reviewed-raw-boundary-or-non-service-usage',
    removalGate: 'SPEC-03',
    finalGate: 'V11-M9-DOCS',
    targetOwners: [
      'core/doc/spec/core/09-runtime-boundary.ko.md',
      'core/doc/internals/runtime-boundary.ko.md',
    ],
    files: [
      'core/doc/guide/03-2-pubsub.md',
      'core/doc/guide/STYLE.ko.md',
      'core/doc/internals/core-source-layout.ko.md',
      'core/doc/internals/posd-module-structure.ko.md',
      'core/doc/internals/posd-module-structure.md',
      'core/doc/internals/protocol-zmp.ko.md',
      'core/doc/internals/protocol-zmp.md',
      'core/doc/internals/runtime-boundary.ko.md',
      'core/doc/internals/runtime-boundary.md',
      'core/doc/internals/thread-safety.ko.md',
      'core/doc/internals/thread-safety.md',
      'core/doc/internals/threading-model.ko.md',
      'core/doc/internals/threading-model.md',
      'core/doc/spec/README.ko.md',
      'core/doc/spec/README.md',
      'core/doc/spec/core/00-public-contract-governance.ko.md',
      'core/doc/spec/core/00-public-contract-governance.md',
      'core/doc/spec/core/05-events.ko.md',
      'core/doc/spec/core/05-events.md',
      'core/doc/spec/core/09-runtime-boundary.ko.md',
      'core/doc/spec/core/09-runtime-boundary.md',
      'core/doc/spec/core/README.ko.md',
      'core/doc/spec/core/README.md',
    ],
  },
];

const coreServiceDocumentationReviews = new Map();
function canonicalCoreDocumentationReviewPath(file) {
  if (fs.existsSync(path.join(repositoryRoot, file))) return file;
  if (file.endsWith('.md') && !file.endsWith('.ko.md')) {
    const english = `${file.slice(0, -3)}.en.md`;
    if (fs.existsSync(path.join(repositoryRoot, english))) return english;
  }
  return file;
}
for (const group of coreServiceDocumentationReviewGroups) {
  for (const file of group.files) {
    const canonicalFile = canonicalCoreDocumentationReviewPath(file);
    if (coreServiceDocumentationReviews.has(canonicalFile)) {
      throw new Error(`duplicate Core service documentation review: ${canonicalFile}`);
    }
    const {files, ...review} = group;
    coreServiceDocumentationReviews.set(canonicalFile, {file: canonicalFile, ...review});
  }
}

const coreServiceDocumentationMarker = new RegExp([
  '\\bservices?\\b',
  '\\bSPOT\\b',
  '\\bActor\\b',
  '\\bMeshNode\\b',
  '\\bStreamSession\\b',
  '\\bstream[ _-]session\\b',
  '\\b(?:source|target)_spot_rid\\b',
  '\\bzlink_(?:spot|actor|mesh_node|stream_session)',
  '\\bZLINK_(?:SPOT|ACTOR|MESH|STREAM_SESSION)',
  '(?:^|[/\\s(])07-(?:0-services|3-spot|4-actor)',
].join('|'), 'iu');

const zmpHeartbeatMarker = new RegExp([
  'ZLINK_(?:INTERNAL_)?OPT_HEARTBEAT_(?:IVL|TTL|TIMEOUT)',
  '(?:^|[^A-Z0-9_])HEARTBEAT_(?:IVL|TTL|TIMEOUT)(?![A-Z0-9_])',
  '\\bHeartbeat(?:Interval|Ttl|Timeout)\\b',
  '\\bheartbeat(?:Interval|Ttl|Timeout)\\b',
  '\\b(?:process_)?heartbeat_(?:ctx|interval|ivl|message|timer|ttl|timeout)',
  '\\bzmp_control_heartbeat(?:_ack)?\\b',
  '\\b(?:build|parse)_heartbeat(?:_ack|_ping)?\\b',
  '\\bzmp_effective_ttl_ds\\b',
].join('|'), 'u');

const bindingDefinitions = {
  cpp: {
    root: 'bindings/cpp',
    publicRoots: [
      'bindings/cpp/include/zlink/Contracts/Service/',
      'bindings/cpp/include/zlink/service/',
    ],
    exactOwner: 'framework/doc/framework/common/spec/server/languages/cpp/interfaces/README.ko.md',
    removalGate: 'V11-M4-BIND-CPP',
    finalGate: 'V11-M9-PKG-CPP',
  },
  dotnet: {
    root: 'bindings/dotnet',
    publicRoots: ['bindings/dotnet/src/Zlink/Contracts/Service/'],
    exactOwner: 'framework/doc/framework/common/spec/server/languages/dotnet/interfaces/README.ko.md',
    removalGate: 'V11-M4-BIND-DN',
    finalGate: 'V11-M9-PKG-DN',
  },
  java: {
    root: 'bindings/java',
    publicRoots: ['bindings/java/src/main/java/systems/zlink/contracts/service/'],
    exactOwner: 'framework/doc/framework/common/spec/server/languages/java/interfaces/README.ko.md',
    additionalOwner: 'framework/doc/framework/common/spec/server/languages/kotlin/interfaces/README.ko.md',
    removalGate: 'V11-M4-BIND-JVM',
    finalGate: 'V11-M9-PKG-JVM',
  },
  node: {
    root: 'bindings/node',
    publicRoots: ['bindings/node/src/zlink/contracts/service/'],
    exactOwner: 'framework/doc/framework/common/spec/server/languages/node/interfaces/README.ko.md',
    removalGate: 'V11-M4-BIND-NODE',
    finalGate: 'V11-M9-PKG-NODE',
  },
};

const frameworkDefinitions = {
  cpp: {
    root: 'framework/languages/cpp',
    auditFile: 'framework/languages/cpp/CMakeLists.txt',
    removalGate: 'V11-M8-CLEAN-CPP',
    finalGate: 'V11-M9-PKG-CPP',
  },
  dotnet: {
    root: 'framework/languages/dotnet',
    auditFile: 'framework/languages/dotnet/src/Zlink.Framework/Zlink.Framework.csproj',
    removalGate: 'V11-M8-CLEAN-DN',
    finalGate: 'V11-M9-PKG-DN',
  },
  java: {
    root: 'framework/languages/java',
    auditFile: 'framework/languages/java/zlink-framework-core/build.gradle.kts',
    removalGate: 'V11-M8-CLEAN-JVM',
    finalGate: 'V11-M9-PKG-JVM',
  },
  node: {
    root: 'framework/languages/node',
    auditFile: 'framework/languages/node/packages/framework/package.json',
    removalGate: 'V11-M8-CLEAN-NODE',
    finalGate: 'V11-M9-PKG-NODE',
  },
};

const frameworkAdjacentComponentPrefixes = {
  cpp: [
    'framework/languages/cpp/connector/',
    'framework/languages/cpp/http-client/',
    'framework/languages/cpp/tests/Systems.Zlink.Stream.Connector.Tests/',
    'framework/languages/cpp/tests/Zlink.Unreal.Stream.Connector.Tests/',
  ],
  dotnet: [
    'framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/',
    'framework/languages/dotnet/src/Zlink.HttpClient/',
    'framework/languages/dotnet/tests/Systems.Zlink.Stream.Connector',
    'framework/languages/dotnet/tests/Zlink.HttpClient',
  ],
  java: [
    'framework/languages/java/zlink-http-client/',
    'framework/languages/java/zlink-http-client-kotlin/',
    'framework/languages/java/zlink-stream-connector/',
  ],
  node: [
    'framework/languages/node/packages/http-client/',
    'framework/languages/node/packages/stream-connector/',
    'framework/languages/node/packages/stream-wire/',
  ],
};

const legacyBindingRoots = {
  c: 'bindings/c',
  go: 'bindings/go',
  python: 'bindings/python',
  rust: 'bindings/rust',
};

const packageInputPrefixes = [
  'scripts/local-package/bindings-candidate/',
  'scripts/local-package/core/',
  'scripts/local-package/cpp/',
  'scripts/local-package/dotnet/',
  'scripts/local-package/framework/',
  'scripts/local-package/java/',
  'scripts/local-package/native/',
  'scripts/local-package/node/',
];

const packageInputFiles = new Set([
  '.github/workflows/bindings-release.yml',
  '.github/workflows/build.yml',
  '.github/workflows/core-conan-release.yml',
  '.github/workflows/framework-dotnet.yml',
  '.github/workflows/framework-node.yml',
  'scripts/local-package/README.ko.md',
  'scripts/local-package/build-windows.ps1',
  'scripts/local-package/build-wsl.sh',
  'scripts/local-package/publish-all-wsl.sh',
]);

const bindingRootMetadata = new Set([
  'bindings/cpp/CMakeLists.txt',
  'bindings/cpp/README.doxygen.md',
  'bindings/cpp/build.sh',
  'bindings/dotnet/Directory.Build.targets',
  'bindings/dotnet/README.docfx.md',
  'bindings/dotnet/README.md',
  'bindings/dotnet/Zlink.sln',
  'bindings/dotnet/src/Zlink/Zlink.csproj',
  'bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj',
  'bindings/java/README.javadoc.md',
  'bindings/java/build.gradle',
  'bindings/java/settings.gradle',
  'bindings/java/src/main/java/module-info.java',
  'bindings/node/README.md',
  'bindings/node/README.typedoc.md',
  'bindings/node/binding.gyp',
  'bindings/node/package-lock.json',
  'bindings/node/package.json',
  'bindings/node/src/index.ts',
  'bindings/node/tsconfig.json',
  'bindings/node/tsconfig.src-review.json',
  'bindings/node/tsconfig.tools.json',
  'bindings/node/tsconfig.typecheck.json',
]);

const binaryPackagePattern = /(?:^|\/)(?:native|prebuilds)(?:\/|$).*(?:libzlink[^/]*|zlink\.(?:dll|node)|libzlink\.dylib)$/u;
const generatedPathPattern = /(?:^|\/)(?:dist-tools|generated|prebuilds)(?:\/|$)/u;
const buildOutputPattern = /(?:^|\/)(?:build|build[-_][^/]+|node_modules|target|obj|bin)(?:\/|$)/u;

const serviceMarker = new RegExp([
  'zlink_(?:mesh(?:_node|_ready|_receive|_claim|_reply|_operation|_monitor|_publish|_destination|_owner|_record|_metadata)?|spot|actor|instance_spot|stream_session)',
  'ZLINK_(?:MESH|SPOT|ACTOR|INSTANCE_SPOT|STREAM_SESSION)',
  'zlink_socket_(?:set|get)_channel_name',
  'ZLINK_POLLER_SOURCE_MESH_NODE',
  '\\b(?:MeshNode|SpotNode|InstanceSpot|StreamSession|ActorTransfer|ActorReceived|ActorRecvInfo|ReadyBatch|ReceiveBatch|ReplyToken|SpotKind)\\b',
  '\\b(?:mesh_node|stream_session|instance_spot|actor_transfer|ready_batch|receive_batch|reply_token)_t\\b',
  'Contracts[\\/]Service',
  'contracts[\\/.]service',
  'Runtime[\\/]Service',
  'runtime[\\/]service',
  'NativeServiceSymbols|ServiceInterop|ServiceLayouts',
  'addon_mesh_service|binding_service',
  'zlink[\\/]service[\\/]',
  'service_control_runtime|runtime[\\/]services[\\/]control',
  'spot_timer_(?:tick_allowed|enter_turn|leave_turn)',
  '\\boption_target_service\\b|\\bstream_session_owns_socket\\b',
  '\\b(?:send|recv)_family_spot(?:_[a-z0-9_]+)?\\b',
  '\\b(?:source|target)_spot_rid\\b|\\brouter_request_reply_state\\b',
  '\\b(?:set|get|ensure|lock|has)_channel_name_metadata\\b',
  'perf_(?:multi_)?spot|SPOT_(?:PUBSUB|REQREP|SENDSEND)|MULTI_SPOT',
  'ZLINK_(?:INTERNAL_)?OPT_HEARTBEAT_(?:IVL|TTL|TIMEOUT)',
  '(?:^|[^A-Z0-9_])HEARTBEAT_(?:IVL|TTL|TIMEOUT)(?![A-Z0-9_])',
  '\\bHeartbeat(?:Interval|Ttl|Timeout)\\b',
  '\\bheartbeat(?:Interval|Ttl|Timeout)\\b',
  '\\b(?:process_)?heartbeat_(?:ctx|interval|ivl|message|timer|ttl|timeout)',
  '\\bzmp_control_heartbeat(?:_ack)?\\b',
  '\\b(?:build|parse)_heartbeat(?:_ack|_ping)?\\b',
  '\\bzmp_effective_ttl_ds\\b',
].join('|'), 'iu');

// Some language projections place service identifiers inside a larger
// identifier, for example CreateMeshNode or NativeInstanceSpotModels. The
// broad marker above intentionally uses word boundaries because it scans many
// mixed files. This narrower marker is only used after a supported binding
// file has matched one of the structural locations below.
const bindingEmbeddedServiceIdentifierMarker = new RegExp([
  '(?:MeshNode|InstanceSpot|StreamSession|Actor|Spot)(?=[A-Z0-9_]|\\b)',
  '(?:^|[^a-z0-9])(?:mesh_node|instance_spot|stream_session)(?![a-z0-9])',
  '(?:^|[^a-z0-9])(?:actor|spot)_(?:ref|transfer|control|join|leave|gateway|contract|message|membership|lifecycle|received|recv|node|kind|record|address|handle|timer|pubsub|rpc|queue|room|sequential|single_player|dispatch|request|response|operation|options|state|route|send|ready|claim)(?![a-z0-9])',
].join('|'), 'mu');

const bindingServiceFactoryMarker = new RegExp([
  '(?:create|Create|new|New|open|Open)(?:MeshNode|InstanceSpot|StreamSession|Actor|Spot)(?=[A-Z0-9_]|\\b)',
  '(?:^|[^a-z0-9])(?:create|new|open)_(?:mesh_node|instance_spot|stream_session|actor|spot)(?![a-z0-9])',
].join('|'), 'mu');

const bindingPublicContextOrFactoryPath =
  /\/(?:Contracts|contracts)\/(?:Core|core)\/[^/]*(?:Context|context|Factory|factory)[^/]*\.[^/]+$/u;
const bindingNativeProjectionPath =
  /\/(?:Runtime|runtime)\/(?:Native|native)(?:\/|$)/u;
const bindingTestPath = /(?:^|\/)(?:tests?|src\/test)(?:\/|$)/u;
const bindingSamplePath = /(?:^|\/)(?:samples?|examples?)(?:\/|$)/u;

function bindingStructuralSource(file, reader = readText) {
  return `${file}\n${stripComments(reader(file))}`;
}

function isBindingPublicServiceFactory(file, reader = readText) {
  return bindingPublicContextOrFactoryPath.test(file)
    && bindingServiceFactoryMarker.test(stripComments(reader(file)));
}

function isBindingNativeServiceProjection(file, reader = readText) {
  return bindingNativeProjectionPath.test(file)
    && bindingEmbeddedServiceIdentifierMarker.test(bindingStructuralSource(file, reader));
}

function isBindingServiceTest(file, reader = readText) {
  return bindingTestPath.test(file)
    && bindingEmbeddedServiceIdentifierMarker.test(bindingStructuralSource(file, reader));
}

function isBindingServiceSample(file, reader = readText) {
  return bindingSamplePath.test(file)
    && bindingEmbeddedServiceIdentifierMarker.test(bindingStructuralSource(file, reader));
}

const coreFunctionPattern = /^(?:zlink_(?:mesh(?:_node|_ready|_receive|_claim|_reply)|spot|actor|instance_spot|stream_session)|zlink_(?:set|get)_mesh_node_option|zlink_socket_(?:set|get)_channel_name)/u;

const supportSymbolPattern = /^(?:zlink_(?:spot_timer_new|mesh_node_monitor_|mesh_monitor_|socket_(?:set|get)_channel_name)|ZLINK_MESH_MONITOR_|ZLINK_POLLER_SOURCE_MESH_NODE|ZLINK_OPT_HEARTBEAT_(?:IVL|TTL|TIMEOUT))/u;

const textExtensions = new Set([
  '.c', '.cc', '.cmake', '.cpp', '.cs', '.csproj', '.gradle', '.gyp', '.h',
  '.go', '.hpp', '.java', '.js', '.json', '.kt', '.kts', '.md', '.mjs',
  '.props', '.ps1', '.py', '.rs', '.sh', '.targets', '.toml', '.ts', '.txt',
  '.xml', '.yaml', '.yml',
  '.vers',
]);

function relativePath(absolute) {
  return path.relative(repositoryRoot, absolute).split(path.sep).join('/');
}

function gitFiles(arguments_) {
  const result = spawnSync(
    'git',
    ['-C', repositoryRoot, 'ls-files', ...arguments_, '-z'],
    {maxBuffer: 64 * 1024 * 1024});
  if (result.error) {
    throw new Error(`git ls-files could not start: ${result.error.message}`);
  }
  if (result.status !== 0) {
    throw new Error(`git ls-files failed: ${result.stderr?.toString('utf8').trim() ?? ''}`);
  }
  return result.stdout.toString('utf8').split('\0').filter(Boolean);
}

function readRepositoryFiles() {
  const tracked = gitFiles(['--cached']);
  const untracked = gitFiles(['--others', '--exclude-standard'])
    .filter(file => !isBinaryPackage(file)
      && !generatedPathPattern.test(file)
      && !buildOutputPattern.test(file)
      && !/^core\/builds\/(?!ci\/|cmake\/|common\/|linux\/|macos\/|windows\/)[^/]+\//u.test(file));
  return [...new Set([...tracked, ...untracked])]
    .filter(file => fs.existsSync(path.join(repositoryRoot, file)))
    .sort((left, right) => left.localeCompare(right, 'en'));
}

function isTextFile(file) {
  const base = path.posix.basename(file);
  return textExtensions.has(path.posix.extname(file).toLowerCase())
    || base === 'CMakeLists.txt'
    || base === 'Makefile';
}

const textCache = new Map();
function readText(file) {
  if (!isTextFile(file)) return '';
  if (!textCache.has(file)) {
    const data = fs.readFileSync(path.join(repositoryRoot, file));
    textCache.set(file, data.includes(0) ? '' : data.toString('utf8'));
  }
  return textCache.get(file);
}

function stripComments(source) {
  return source
    .replace(/\/\*[\s\S]*?\*\//gu, match => match.replace(/[^\n]/gu, ' '))
    .replace(/\/\/[^\n]*/gu, '');
}

function normalizeSpace(value) {
  return value.replace(/\s+/gu, ' ').trim();
}

function splitCommaList(body) {
  const values = [];
  let start = 0;
  let round = 0;
  let square = 0;
  let brace = 0;
  let angle = 0;
  for (let index = 0; index < body.length; ++index) {
    const character = body[index];
    if (character === '(') ++round;
    else if (character === ')') --round;
    else if (character === '[') ++square;
    else if (character === ']') --square;
    else if (character === '{') ++brace;
    else if (character === '}') --brace;
    else if (character === '<') ++angle;
    else if (character === '>' && angle > 0) --angle;
    else if (character === ',' && round === 0 && square === 0 && brace === 0 && angle === 0) {
      values.push(body.slice(start, index));
      start = index + 1;
    }
  }
  values.push(body.slice(start));
  return values;
}

function extractStructFields(body) {
  const fields = [];
  for (const rawDeclaration of body.split(';')) {
    const declaration = normalizeSpace(rawDeclaration.replace(/^\s*#.*$/gmu, ''));
    if (!declaration) continue;
    const callback = declaration.match(/\(\s*\*\s*([A-Za-z_]\w*)\s*\)/u);
    const ordinary = declaration.match(/([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$/u);
    const name = callback?.[1] ?? ordinary?.[1];
    if (!name) throw new Error(`cannot classify public struct field: ${declaration}`);
    fields.push({name, declaration});
  }
  return fields;
}

function publicHeaderSymbols(file, forceAll = false, reader = readText) {
  const source = reader(file);
  const stripped = stripComments(source);
  const serviceHeader = forceAll || coreServiceHeaders.includes(file);
  const includeSymbol = (name, parent = '') => serviceHeader
    || supportSymbolPattern.test(name)
    || supportSymbolPattern.test(parent);
  const symbols = [];
  const occupied = [];

  for (const match of stripped.matchAll(/^\s*#define\s+(ZLINK_[A-Z0-9_]+)(?:\([^)]*\))?\s*([^\n]*)/gmu)) {
    const name = match[1];
    if (name.endsWith('_H_INCLUDED')
        || name.endsWith('_H_INCLUDED__')
        || name === 'ZLINK_EXPORT'
        || name === 'ZLINK_INSTANCE_SPOT_DRIVER_RESTORED_EXPORT') continue;
    if (includeSymbol(name)) {
      symbols.push({kind: 'macro', symbol: name, value: normalizeSpace(match[2])});
    }
  }

  const enumPattern = /typedef\s+enum\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\}\s*([A-Za-z_]\w*)\s*;/gu;
  for (const match of stripped.matchAll(enumPattern)) {
    occupied.push([match.index, match.index + match[0].length]);
    const tag = match[1];
    const alias = match[3];
    if (includeSymbol(alias, tag)) {
      symbols.push({kind: 'enum-type', symbol: alias, tag});
    }
    for (const rawValue of splitCommaList(match[2])) {
      const value = normalizeSpace(rawValue);
      if (!value) continue;
      const valueMatch = value.match(/^([A-Za-z_]\w*)\s*(?:=\s*([\s\S]*))?$/u);
      if (!valueMatch) throw new Error(`cannot classify enum value in ${file}: ${value}`);
      if (includeSymbol(valueMatch[1], alias)) {
        symbols.push({
          kind: 'enum-value',
          symbol: valueMatch[1],
          parent: alias,
          value: normalizeSpace(valueMatch[2] ?? ''),
        });
      }
    }
  }

  const compositePattern = /typedef\s+(struct|union)\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\}\s*([A-Za-z_]\w*)\s*;/gu;
  for (const match of stripped.matchAll(compositePattern)) {
    occupied.push([match.index, match.index + match[0].length]);
    const compositeKind = match[1];
    const tag = match[2];
    const alias = match[4];
    if (!includeSymbol(alias, tag)) continue;
    symbols.push({kind: `${compositeKind}-type`, symbol: alias, tag});
    for (const field of extractStructFields(match[3])) {
      symbols.push({
        kind: `${compositeKind}-field`,
        symbol: field.name,
        parent: alias,
        declaration: field.declaration,
      });
    }
  }

  const callbackPattern = /typedef\s+([^;{}]*?)\(\s*\*\s*(zlink_[a-z0-9_]+)\s*\)\s*\(([^;{}]*?)\)\s*;/gu;
  for (const match of stripped.matchAll(callbackPattern)) {
    occupied.push([match.index, match.index + match[0].length]);
    if (!includeSymbol(match[2])) continue;
    symbols.push({
      kind: 'callback',
      symbol: match[2],
      signature: normalizeSpace(match[0]),
    });
  }

  const withoutOccupied = [...stripped];
  for (const [begin, end] of occupied) {
    for (let index = begin; index < end; ++index) {
      if (withoutOccupied[index] !== '\n') withoutOccupied[index] = ' ';
    }
  }
  const aliasSource = withoutOccupied.join('');
  for (const match of aliasSource.matchAll(/typedef\s+([^;{}]+?)\s+(zlink_[a-z0-9_]+)\s*;/gu)) {
    if (!includeSymbol(match[2])) continue;
    symbols.push({
      kind: 'type-alias',
      symbol: match[2],
      declaration: normalizeSpace(match[0]),
    });
  }

  const anonymousEnumPattern = /\benum\s*\{([\s\S]*?)\}\s*;/gu;
  for (const match of aliasSource.matchAll(anonymousEnumPattern)) {
    const values = splitCommaList(match[1])
      .map(normalizeSpace)
      .filter(Boolean);
    const firstName = values[0]?.match(/^([A-Za-z_]\w*)/u)?.[1] ?? 'unnamed';
    const parent = `anonymous:${firstName}`;
    for (const value of values) {
      const valueMatch = value.match(/^([A-Za-z_]\w*)\s*(?:=\s*([\s\S]*))?$/u);
      if (!valueMatch) throw new Error(`cannot classify anonymous enum value in ${file}: ${value}`);
      if (!includeSymbol(valueMatch[1], parent)) continue;
      symbols.push({
        kind: 'enum-value',
        symbol: valueMatch[1],
        parent,
        value: normalizeSpace(valueMatch[2] ?? ''),
      });
    }
  }

  const functionPattern = /^(?!\s*#)\s*ZLINK_EXPORT\s+([\s\S]*?)\b(zlink_[a-z0-9_]+)\s*\(([\s\S]*?)\)\s*;/gmu;
  for (const match of stripped.matchAll(functionPattern)) {
    if (!includeSymbol(match[2])) continue;
    symbols.push({
      kind: 'function',
      symbol: match[2],
      signature: normalizeSpace(match[0]),
    });
  }

  const key = symbol => [symbol.kind, symbol.parent ?? '', symbol.symbol].join('\u0000');
  const unique = new Map();
  for (const symbol of symbols) {
    const symbolKey = key(symbol);
    if (unique.has(symbolKey)) {
      throw new Error(`duplicate public header symbol ${symbol.symbol} in ${file}`);
    }
    unique.set(symbolKey, symbol);
  }
  return [...unique.values()].sort((left, right) => key(left).localeCompare(key(right), 'en'));
}

function symbolTargetOwners(symbol) {
  const value = `${symbol.parent ?? ''} ${symbol.symbol}`.toLowerCase();
  if (value.includes('heartbeat')) {
    return [
      'core/doc/spec/core/09-runtime-boundary.ko.md',
      'core/doc/internals/runtime-boundary.ko.md',
      `${formalServerSpecRoot}/55-transport-liveness.ko.md`,
      `${commonInternalsRoot}/10-liveness-and-state.ko.md`,
    ];
  }
  if (value.includes('instance_spot')) {
    return [
      `${formalServerSpecRoot}/24-spot-address-messaging.ko.md`,
      `${commonInternalsRoot}/05-relocation-continuity.ko.md`,
    ];
  }
  if (value.includes('stream_session') || value.includes('bound_session')) {
    return [
      `${formalServerSpecRoot}/30-stream-session.ko.md`,
      `${commonInternalsRoot}/09-session-binding.ko.md`,
    ];
  }
  if (value.includes('actor')) {
    return [
      `${formalServerSpecRoot}/22-actor-model.ko.md`,
      `${formalServerSpecRoot}/23-spot-actor.ko.md`,
      `${commonInternalsRoot}/05-relocation-continuity.ko.md`,
    ];
  }
  if (value.includes('spot')) {
    return [
      `${formalServerSpecRoot}/20-spot-messaging.ko.md`,
      `${commonInternalsRoot}/05-relocation-continuity.ko.md`,
    ];
  }
  if (value.includes('monitor')) {
    return [
      `${formalServerSpecRoot}/50-runtime-monitoring.ko.md`,
      `${commonInternalsRoot}/10-liveness-and-state.ko.md`,
    ];
  }
  if (/ready|receive|claim|reply|record|operation|destination|owner/u.test(value)) {
    return [
      `${formalSpecRoot}/04-async-execution-policy.ko.md`,
      `${commonInternalsRoot}/07-dispatch-loop.ko.md`,
    ];
  }
  return [
    `${formalServerSpecRoot}/21-mesh-node.ko.md`,
    `${commonInternalsRoot}/README.ko.md`,
    `${commonInternalsRoot}/service-wire-protocol.ko.md`,
  ];
}

function symbolDisposition(file, symbol) {
  const value = `${symbol.parent ?? ''} ${symbol.symbol}`.toLowerCase();
  if (value.includes('heartbeat')) return 'remove';
  if (file.endsWith('/dispatch.h')) return 'superseded';
  if (symbol.symbol === 'ZLINK_POLLER_SOURCE_MESH_NODE') return 'remove';
  if (symbol.kind === 'macro' && symbol.symbol.endsWith('_ABI_VERSION')) return 'remove';
  if (file.endsWith('/instance_spot_driver.h')) return 'superseded';
  if (/struct_size|(^|_)version$|opaque/u.test(symbol.symbol.toLowerCase())) return 'superseded';
  if (/claim|batch|reply_token|activation_token|transfer_token|transfer_prepare|transfer_control/u.test(value)) {
    return 'superseded';
  }
  if (symbol.kind === 'function'
      && /drain_ready|set_ready_handler|mesh_reply|transfer_(?:prepare|commit|activate|abort)/u.test(value)) {
    return 'superseded';
  }
  return 'target-contract';
}

function discoverCurrentCorePublicSymbolRecords() {
  const records = [];
  for (const file of [...coreServiceHeaders, ...coreSupportHeaders]) {
    if (!fs.existsSync(path.join(repositoryRoot, file))) {
      if (coreServiceHeaders.includes(file)) continue;
      throw new Error(`required raw public header is missing: ${file}`);
    }
    const symbols = publicHeaderSymbols(file);
    if (coreServiceHeaders.includes(file) && symbols.length === 0) {
      throw new Error(`service header has no classified public symbols: ${file}`);
    }
    for (const symbol of symbols) {
      const parent = symbol.parent ? `:${symbol.parent}` : '';
      records.push({
        id: `core-public-symbol:${file}:${symbol.kind}${parent}:${symbol.symbol}`,
        file,
        ...symbol,
        disposition: symbolDisposition(file, symbol),
        action: 'remove-symbol-from-core-11',
        targetOwners: symbolTargetOwners(symbol),
        removalGate: 'V11-M3-CORE-REMOVE',
        finalGate: 'V11-M9-RAW-FINAL',
      });
    }
  }
  return records.sort((left, right) => left.id.localeCompare(right.id, 'en'));
}

function loadReviewedCoreBaseline() {
  if (!fs.existsSync(inventoryPath)) {
    throw new Error([
      `reviewed Core service baseline is missing: ${relativePath(inventoryPath)}`,
      'Restore the reviewed inventory; --write must not reconstruct removed Core service history from the current tree.',
    ].join('\n'));
  }
  let inventory;
  try {
    inventory = JSON.parse(fs.readFileSync(inventoryPath, 'utf8'));
  } catch (error) {
    throw new Error(`reviewed Core service baseline is not valid JSON: ${error.message}`);
  }
  const baseline = {
    corePublicSymbols: inventory.corePublicSymbols,
    coreExportSymbols: inventory.coreExportSymbols,
  };
  if (!Array.isArray(baseline.corePublicSymbols)
      || !Array.isArray(baseline.coreExportSymbols)) {
    throw new Error('reviewed Core service baseline sections are missing');
  }
  const digest = crypto.createHash('sha256').update(stableJson(baseline)).digest('hex');
  if (digest !== reviewedCoreBaselineSha256) {
    throw new Error([
      'reviewed Core service baseline digest differs from the sealed value',
      `expected=${reviewedCoreBaselineSha256}`,
      `actual=${digest}`,
    ].join('\n'));
  }
  return {
    corePublicSymbols: relocateConsolidatedDocumentOwners(baseline.corePublicSymbols),
    coreExportSymbols: relocateConsolidatedDocumentOwners(baseline.coreExportSymbols),
  };
}

function selectReviewedCoreRemovalFiles(inventory) {
  return (inventory.files ?? []).filter(record => record.scope === 'core'
    && ['V11-M3-CORE-REMOVE', 'V11-M3-CORE-CLEAN'].includes(record.removalGate));
}

function verifyReviewedCoreRemovalFiles(records, source) {
  const digest = crypto.createHash('sha256').update(stableJson(records)).digest('hex');
  if (digest !== reviewedCoreRemovalFilesSha256) {
    throw new Error([
      `reviewed Core removal-file baseline differs at ${source}`,
      `expected=${reviewedCoreRemovalFilesSha256}`,
      `actual=${digest}`,
    ].join('\n'));
  }
  return records;
}

function relocateConsolidatedDocumentOwners(records) {
  const relocated = new Map([
    ['framework/doc/plan/v11.0/route-mesh-11.0.0-execution-ledger.ko.md', ledgerPath],
    [`${commonInternalsRoot}/service-runtime-architecture.ko.md`,
      `${commonInternalsRoot}/README.ko.md`],
    [`${commonInternalsRoot}/mailbox-dispatch-runtime.ko.md`,
      `${commonInternalsRoot}/07-dispatch-loop.ko.md`],
    [`${commonInternalsRoot}/stateful-maintenance-runtime.ko.md`,
      `${commonInternalsRoot}/05-relocation-continuity.ko.md`],
    [`${commonInternalsRoot}/stream-session-runtime.ko.md`,
      `${commonInternalsRoot}/09-session-binding.ko.md`],
    [`${commonInternalsRoot}/transport-liveness-runtime.ko.md`,
      `${commonInternalsRoot}/10-liveness-and-state.ko.md`],
    [`${commonInternalsRoot}/service-monitoring-runtime.ko.md`,
      `${commonInternalsRoot}/10-liveness-and-state.ko.md`],
    [`${commonInternalsRoot}/concurrency-resource-runtime.ko.md`,
      `${commonInternalsRoot}/02-serialization.ko.md`],
    [`${commonInternalsRoot}/routing-identity-runtime.ko.md`,
      `${commonInternalsRoot}/06-routing-and-cache.ko.md`],
    [`${retiredPlanSpecRoot}/README.ko.md`, formalSpecIndex],
    [`${retiredPlanSpecRoot}/01-mesh-node.ko.md`,
      `${formalServerSpecRoot}/21-mesh-node.ko.md`],
    [`${retiredPlanSpecRoot}/02-dispatch.ko.md`,
      `${formalSpecRoot}/04-async-execution-policy.ko.md`],
    [`${retiredPlanSpecRoot}/03-spot.ko.md`,
      `${formalServerSpecRoot}/20-spot-messaging.ko.md`],
    [`${retiredPlanSpecRoot}/04-actor.ko.md`,
      `${formalServerSpecRoot}/22-actor-model.ko.md`],
    [`${retiredPlanSpecRoot}/05-stream-session.ko.md`,
      `${formalServerSpecRoot}/30-stream-session.ko.md`],
    [`${retiredPlanSpecRoot}/06-instance-spot.ko.md`,
      `${formalServerSpecRoot}/24-spot-address-messaging.ko.md`],
    [`${retiredPlanSpecRoot}/07-location-maintenance.ko.md`,
      `${formalServerSpecRoot}/40-location-runtime.ko.md`],
    [`${retiredPlanSpecRoot}/08-liveness-observability.ko.md`,
      `${formalServerSpecRoot}/50-runtime-monitoring.ko.md`],
    [`${retiredPlanSpecRoot}/09-core-raw-runtime-boundary.ko.md`,
      'core/doc/spec/core/09-runtime-boundary.ko.md'],
    [`${retiredPlanInternalsRoot}/README.ko.md`, commonInternalsIndex],
    [`${retiredPlanInternalsRoot}/01-runtime-architecture.ko.md`,
      `${commonInternalsRoot}/README.ko.md`],
    [`${retiredPlanInternalsRoot}/02-wire-protocol.ko.md`,
      `${commonInternalsRoot}/service-wire-protocol.ko.md`],
    [`${retiredPlanInternalsRoot}/03-mailbox-dispatch.ko.md`,
      `${commonInternalsRoot}/07-dispatch-loop.ko.md`],
    [`${retiredPlanInternalsRoot}/04-stateful-object-runtime.ko.md`,
      `${commonInternalsRoot}/05-relocation-continuity.ko.md`],
    [`${retiredPlanInternalsRoot}/05-maintenance-recovery.ko.md`,
      `${commonInternalsRoot}/05-relocation-continuity.ko.md`],
    [`${retiredPlanInternalsRoot}/06-stream-session-runtime.ko.md`,
      `${commonInternalsRoot}/09-session-binding.ko.md`],
    [`${retiredPlanInternalsRoot}/07-liveness-monitoring.ko.md`,
      `${commonInternalsRoot}/10-liveness-and-state.ko.md`],
    [`${retiredPlanInternalsRoot}/08-concurrency-resources.ko.md`,
      `${commonInternalsRoot}/02-serialization.ko.md`],
    [`${retiredPlanInternalsRoot}/09-core-raw-runtime-boundary.ko.md`,
      'core/doc/internals/runtime-boundary.ko.md'],
  ]);
  return records.map(record => ({
    ...record,
    targetOwners: (record.targetOwners || [])
      .map(owner => relocated.get(owner) || owner),
  }));
}

function loadReviewedCoreRemovalFiles() {
  const current = JSON.parse(fs.readFileSync(inventoryPath, 'utf8'));
  const currentRecords = selectReviewedCoreRemovalFiles(current);
  const currentDigest = crypto.createHash('sha256')
    .update(stableJson(currentRecords)).digest('hex');
  if (currentDigest === reviewedCoreRemovalFilesSha256) {
    return relocateConsolidatedDocumentOwners(currentRecords);
  }

  const result = spawnSync('git', [
    '-C', repositoryRoot,
    'show',
    `${reviewedCoreRemovalFilesRevision}:${inventoryRepositoryPath}`,
  ], {maxBuffer: 64 * 1024 * 1024});
  if (result.error || result.status !== 0) {
    throw new Error([
      'reviewed Core removal-file baseline is incomplete in the working inventory',
      `the pinned provenance revision ${reviewedCoreRemovalFilesRevision} is unavailable`,
      result.error?.message ?? result.stderr?.toString('utf8').trim() ?? '',
    ].filter(Boolean).join('\n'));
  }
  let pinned;
  try {
    pinned = JSON.parse(result.stdout.toString('utf8'));
  } catch (error) {
    throw new Error(`pinned Core removal-file baseline is not valid JSON: ${error.message}`);
  }
  return relocateConsolidatedDocumentOwners(verifyReviewedCoreRemovalFiles(
    selectReviewedCoreRemovalFiles(pinned),
    `${reviewedCoreRemovalFilesRevision}:${inventoryRepositoryPath}`));
}

function recordIdentity(record) {
  return JSON.stringify(record);
}

function validateCoreSurfaceState(baseline, current, label) {
  const baselineValues = baseline.map(recordIdentity);
  const currentValues = current.map(recordIdentity);
  const matchesBaseline = baselineValues.length === currentValues.length
    && baselineValues.every((value, index) => value === currentValues[index]);
  if (matchesBaseline) return 'reviewed-baseline-present';
  if (current.length === 0) return 'removed';
  const baselineIds = new Set(baseline.map(record => record.id));
  const currentIds = new Set(current.map(record => record.id));
  const added = current.filter(record => !baselineIds.has(record.id)).map(record => record.id);
  const removed = baseline.filter(record => !currentIds.has(record.id)).map(record => record.id);
  const changed = current.filter(record => baselineIds.has(record.id)
    && recordIdentity(record) !== recordIdentity(baseline.find(item => item.id === record.id)))
    .map(record => record.id);
  throw new Error([
    `${label} is neither the sealed baseline nor fully removed`,
    added.length ? `added (${added.length}): ${added.slice(0, 10).join(', ')}` : '',
    removed.length ? `removed (${removed.length}): ${removed.slice(0, 10).join(', ')}` : '',
    changed.length ? `changed (${changed.length}): ${changed.slice(0, 10).join(', ')}` : '',
  ].filter(Boolean).join('\n'));
}

function validateCoreSurfaceStateSelfTests(baseline) {
  if (validateCoreSurfaceState(baseline, baseline, 'self-test') !== 'reviewed-baseline-present'
      || validateCoreSurfaceState(baseline, [], 'self-test') !== 'removed') {
    throw new Error('Core removal-state self-test rejected a valid terminal state');
  }
  for (const mutation of [
    baseline.slice(1),
    [...baseline, {...baseline[0], id: `${baseline[0].id}:negative-mutation`}],
  ]) {
    let rejected = false;
    try {
      validateCoreSurfaceState(baseline, mutation, 'negative mutation');
    } catch {
      rejected = true;
    }
    if (!rejected) throw new Error('Core removal-state negative mutation was not rejected');
  }
}

function exportedServiceSymbolRecords(publicSymbols) {
  const versionScript = 'core/src/libzlink.vers';
  const source = readText(versionScript);
  const publicFunctions = new Set(
    publicSymbols.filter(record => record.kind === 'function').map(record => record.symbol));
  const exported = [...source.matchAll(/^\s*(zlink_[a-z0-9_]+);\s*$/gmu)]
    .map(match => match[1])
    .filter(symbol => coreFunctionPattern.test(symbol))
    .sort((left, right) => left.localeCompare(right, 'en'));
  const exportedSet = new Set(exported);
  const missingExports = [...publicFunctions].filter(symbol => !exportedSet.has(symbol));
  const missingDeclarations = exported.filter(symbol => !publicFunctions.has(symbol));
  if (missingExports.length || missingDeclarations.length) {
    throw new Error([
      missingExports.length ? `public functions missing from libzlink.vers: ${missingExports.join(', ')}` : '',
      missingDeclarations.length ? `service exports missing public declarations: ${missingDeclarations.join(', ')}` : '',
    ].filter(Boolean).join('\n'));
  }
  const publicByName = new Map(
    publicSymbols.filter(record => record.kind === 'function')
      .map(record => [record.symbol, record]));
  return exported.map(symbol => {
    const owner = publicByName.get(symbol);
    return {
      id: `core-export-symbol:${symbol}`,
      file: versionScript,
      kind: 'export-symbol',
      symbol,
      header: owner.file,
      disposition: owner.disposition,
      action: 'remove-export-from-core-11',
      targetOwners: owner.targetOwners,
      removalGate: 'V11-M3-CORE-REMOVE',
      finalGate: 'V11-M9-RAW-FINAL',
    };
  });
}

function isBinaryPackage(file) {
  return binaryPackagePattern.test(file);
}

function languageForFile(file) {
  for (const [language, definition] of Object.entries(bindingDefinitions)) {
    if (file === definition.root || file.startsWith(`${definition.root}/`)) return language;
  }
  return undefined;
}

function frameworkLanguageForFile(file) {
  for (const [language, definition] of Object.entries(frameworkDefinitions)) {
    if (file === definition.root || file.startsWith(`${definition.root}/`)) return language;
  }
  return undefined;
}

function isFrameworkAdjacentComponent(language, file) {
  return frameworkAdjacentComponentPrefixes[language]
    .some(prefix => file.startsWith(prefix));
}

function legacyLanguageForFile(file) {
  for (const [language, root] of Object.entries(legacyBindingRoots)) {
    if (file === root || file.startsWith(`${root}/`)) return language;
  }
  return undefined;
}

function isPublicBindingFile(language, file) {
  return bindingDefinitions[language].publicRoots.some(root => file.startsWith(root));
}

function bindingCandidate(language, file, reader = readText) {
  if (buildOutputPattern.test(file)) return false;
  if (bindingRootMetadata.has(file) || isBinaryPackage(file)) return true;
  if (isPublicBindingFile(language, file)) return true;
  if (/(?:^|\/)(?:Runtime|runtime)\/(?:Service|service)(?:\/|$)/u.test(file)) return true;
  if (isBindingPublicServiceFactory(file, reader)
      || isBindingNativeServiceProjection(file, reader)
      || isBindingServiceTest(file, reader)
      || isBindingServiceSample(file, reader)) return true;
  const source = reader(file);
  return source.length > 0 && serviceMarker.test(source);
}

function legacyCandidate(file, reader = readText) {
  if (buildOutputPattern.test(file)) return false;
  if (isBinaryPackage(file)) return true;
  if (/\/include\/zlink\/service\//u.test(file)) return true;
  const source = reader(file);
  if (source.length > 0 && serviceMarker.test(source)) return true;
  const basename = path.posix.basename(file);
  return /^(?:CMakeLists\.txt|Cargo\.toml|go\.mod|package\.json|pyproject\.toml|setup\.py)$/u.test(basename)
    && file.split('/').length <= 4;
}

function frameworkCandidate(language, file, reader = readText) {
  if (buildOutputPattern.test(file) || !isTextFile(file)) return false;
  if (isFrameworkAdjacentComponent(language, file)) return false;
  if (/(?:^|\/)(?:Runtime|runtime)\/(?:Service|service)(?:\/|$)/u.test(file)) return true;
  const source = reader(file);
  if (!source) return false;
  if (serviceMarker.test(source)) return true;
  for (const header of coreServiceHeaders) {
    const basename = path.posix.basename(header, '.h');
    if (new RegExp(`\\bzlink_${basename}(?:_|\\b)`, 'u').test(source)) return true;
  }
  return false;
}

function discoverUnreviewedCoreServiceDocuments(
  repositoryFiles,
  reviews = coreServiceDocumentationReviews,
  reader = readText) {
  const historical = new Set(historicalServiceDocuments);
  return repositoryFiles.filter(file => file.startsWith('core/doc/')
    && isTextFile(file)
    && !historical.has(file)
    && !reviews.has(file)
    && coreServiceDocumentationMarker.test(reader(file)));
}

function validateCoreServiceDocumentationReviews(repositoryFiles) {
  const allowedDecisions = new Set(['Remove', 'Rewrite', 'Retain']);
  const repositoryFileSet = new Set(repositoryFiles);
  for (const [file, review] of coreServiceDocumentationReviews) {
    if (!allowedDecisions.has(review.reviewDecision)) {
      throw new Error(`Core service documentation review has invalid decision: ${file}`);
    }
    if (!review.action || !review.removalGate || !review.finalGate
        || !Array.isArray(review.targetOwners) || review.targetOwners.length === 0) {
      throw new Error(`Core service documentation review is incomplete: ${file}`);
    }
    if (review.reviewDecision !== 'Remove' && !repositoryFileSet.has(file)) {
      throw new Error(`reviewed Core document is unexpectedly missing: ${file}`);
    }
  }

  const unreviewed = discoverUnreviewedCoreServiceDocuments(repositoryFiles);
  if (unreviewed.length > 0) {
    throw new Error(`unreviewed Core service documentation (${unreviewed.length}):\n  ${unreviewed.join('\n  ')}`);
  }

  const zmpGuide = 'core/doc/guide/zmp-protocol.ko.md';
  const withoutZmpGuide = new Map(coreServiceDocumentationReviews);
  withoutZmpGuide.delete(zmpGuide);
  const removedAllowlistEntry = discoverUnreviewedCoreServiceDocuments(
    [zmpGuide], withoutZmpGuide,
    file => file === zmpGuide ? 'A SPOT routed envelope carries service metadata.' : '');
  if (!removedAllowlistEntry.includes(zmpGuide)) {
    throw new Error('Core document negative mutation did not detect the unreviewed SPOT ZMP envelope');
  }

  const synthetic = 'core/doc/guide/__negative-unreviewed-service__.ko.md';
  const syntheticMutation = discoverUnreviewedCoreServiceDocuments(
    [synthetic], coreServiceDocumentationReviews,
    file => file === synthetic ? 'A MeshNode exposes a SPOT service.' : '');
  if (!syntheticMutation.includes(synthetic)) {
    throw new Error('Core document negative mutation did not detect a new service guide');
  }
}

const completedCoreRawSpecRewriteRules = new Map([
  ['core/doc/spec/core/02-message.ko.md',
    /service[ -]routing|MeshNode|Spot service|service\/03-spot/iu],
  ['core/doc/spec/core/02-message.en.md',
    /service[ -]routing|service specifications?|MeshNode|Spot service|service\/03-spot/iu],
  ['core/doc/spec/core/03-errors.ko.md',
    /service lifecycle|revoked claim|actor join|actor.*이미 존재|mailbox|capacity reservation.*transfer/iu],
  ['core/doc/spec/core/03-errors.en.md',
    /service lifecycle|opaque-token|revoked claim|actor join|actor already exists|mailbox|capacity reservation.*transfer/iu],
  ['core/doc/spec/core/socket/README.ko.md',
    /MeshNode|\bspot\b|actor join|actor.*이미 존재|mailbox|service[ -]owner|ZLINK_OPT_HEARTBEAT_(?:IVL|TTL|TIMEOUT)/iu],
  ['core/doc/spec/core/socket/README.en.md',
    /MeshNode|\bspot\b|actor join|actor already exists|mailbox|service[ -]owner|ZLINK_OPT_HEARTBEAT_(?:IVL|TTL|TIMEOUT)/iu],
  ['core/doc/spec/sample/SAMPLE_POLICY.en.md',
    /MeshNode|ChannelName|\bspot\b|\bactor\b|service[ -]layer|registry query|discovery registry/iu],
]);

function findIncompleteCoreRawSpecRewrites(reader = readText) {
  const findings = [];
  for (const [file, forbiddenPattern] of completedCoreRawSpecRewriteRules) {
    const source = reader(file);
    if (forbiddenPattern.test(source)) findings.push(file);
  }
  return findings;
}

function validateCompletedCoreRawSpecRewrites() {
  const findings = findIncompleteCoreRawSpecRewrites();
  if (findings.length > 0) {
    throw new Error(`Core 11 raw formal spec still contains service or ZMP heartbeat contract text:\n  ${findings.join('\n  ')}`);
  }

  const mutated = findIncompleteCoreRawSpecRewrites(
    file => file === 'core/doc/spec/core/socket/README.en.md'
      ? 'ZLINK_OPT_HEARTBEAT_TIMEOUT remains public.'
      : readText(file));
  if (!mutated.includes('core/doc/spec/core/socket/README.en.md')) {
    throw new Error('Core raw formal-spec negative mutation did not detect a restored ZMP heartbeat option');
  }
}

function coreCandidate(file, reader = readText) {
  if (!file.startsWith('core/') || buildOutputPattern.test(file)) return false;
  if (coreServiceDocumentationReviews.has(file)) return true;
  if (coreServiceHeaders.includes(file)
      || coreSupportHeaders.includes(file)
      || file === 'core/include/zlink.h'
      || file === 'core/src/libzlink.vers') return true;
  if (file.startsWith('core/doc/spec/core/service/')
      || /core\/doc\/internals\/services-internals(?:\.ko)?\.md$/u.test(file)) return true;
  if (file.startsWith('core/src/api/mesh/')
      || file.startsWith('core/src/runtime/services/mesh/')
      || file.startsWith('core/src/runtime/services/control/')) return true;
  const source = reader(file);
  return source.length > 0 && serviceMarker.test(source);
}

function sharedCandidate(file, reader = readText) {
  if (packageInputFiles.has(file)) return true;
  if (packageInputPrefixes.some(prefix => file.startsWith(prefix))) return true;
  if (file.startsWith('bindings/c/perf/')) {
    const source = reader(file);
    return /spot|SPOT_|MULTI_SPOT/iu.test(file) || /zlink_(?:spot|mesh_node)|SPOT_|MULTI_SPOT/u.test(source);
  }
  if (file.startsWith('framework/runtime/') && isTextFile(file)) {
    const source = reader(file);
    return source.length > 0 && serviceMarker.test(source);
  }
  return false;
}

function coreFileRecord(file) {
  const common = {
    id: `file:${file}`,
    file,
    scope: 'core',
    finalGate: 'V11-M9-RAW-FINAL',
  };
  const documentationReview = coreServiceDocumentationReviews.get(file);
  if (documentationReview) {
    return {
      ...common,
      category: documentationReview.category,
      disposition: documentationReview.disposition,
      decision: documentationReview.reviewDecision,
      action: documentationReview.action,
      targetOwners: documentationReview.targetOwners,
      removalGate: documentationReview.removalGate,
      finalGate: documentationReview.finalGate,
    };
  }
  if (file === 'core/doc/spec/core/09-runtime-boundary.ko.md'
      || file === 'core/doc/spec/core/09-runtime-boundary.md'
      || file === 'core/doc/internals/runtime-boundary.ko.md'
      || file === 'core/doc/internals/runtime-boundary.md') {
    return {
      ...common,
      category: file.includes('/internals/') ? 'core-raw-internals' : 'core-raw-spec',
      disposition: 'raw-core-prerequisite',
      action: 'retain-file',
      targetOwners: [file],
      removalGate: 'V11-M3-CORE-CLEAN',
    };
  }
  if (coreServiceHeaders.includes(file)) {
    return {
      ...common,
      category: 'core-service-public-header',
      disposition: 'target-contract',
      action: 'remove-file',
      targetOwners: [`${targetSpecRoot}/README.ko.md`, `${targetInternalsRoot}/README.ko.md`],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (coreSupportHeaders.includes(file) || file === 'core/include/zlink.h') {
    return {
      ...common,
      category: 'core-public-support-header',
      disposition: 'target-contract',
      action: 'remove-service-reference',
      targetOwners: [
        'core/doc/spec/core/09-runtime-boundary.ko.md',
        `${targetSpecRoot}/README.ko.md`,
      ],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (file === 'core/src/libzlink.vers') {
    return {
      ...common,
      category: 'core-export-manifest',
      disposition: 'remove',
      action: 'remove-service-reference',
      targetOwners: ['core/doc/spec/core/09-runtime-boundary.ko.md'],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (file.startsWith('core/src/api/mesh/')) {
    return {
      ...common,
      category: 'core-service-api-source',
      disposition: 'target-contract',
      action: 'remove-file',
      targetOwners: [`${targetInternalsRoot}/README.ko.md`],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (file.startsWith('core/src/runtime/services/mesh/')) {
    return {
      ...common,
      category: 'core-service-runtime-source',
      disposition: 'target-contract',
      action: 'remove-file',
      targetOwners: [`${targetInternalsRoot}/README.ko.md`],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (file.startsWith('core/src/runtime/services/control/')) {
    return {
      ...common,
      category: 'core-generic-control-runtime-input',
      disposition: 'raw-core-prerequisite',
      action: 'retain-or-rename-generic-scheduler-and-remove-service-specific-state',
      targetOwners: [
        'core/doc/internals/runtime-boundary.ko.md',
        `${commonInternalsRoot}/10-liveness-and-state.ko.md`,
      ],
      removalGate: 'V11-M3-CORE-CLEAN',
    };
  }
  if (!file.startsWith('core/doc/') && zmpHeartbeatMarker.test(readText(file))) {
    return {
      ...common,
      category: file.startsWith('core/tests/')
        ? 'core-zmp-heartbeat-test'
        : 'core-zmp-heartbeat-source',
      disposition: 'remove',
      action: 'remove-zmp-heartbeat-option-frame-timer-or-test',
      targetOwners: [
        'core/doc/spec/core/09-runtime-boundary.ko.md',
        'core/doc/internals/runtime-boundary.ko.md',
        `${commonInternalsRoot}/10-liveness-and-state.ko.md`,
      ],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (file.startsWith('core/tests/')) {
    if (/service_control_runtime|unittest_ctx_runtime/u.test(readText(file))) {
      return {
        ...common,
        category: 'core-generic-control-runtime-test',
        disposition: 'raw-core-prerequisite',
        action: 'retain-generic-scheduler-regression-and-remove-service-specific-assertions',
        targetOwners: ['core/doc/internals/runtime-boundary.ko.md', ledgerPath],
        removalGate: 'V11-M3-CORE-CLEAN',
      };
    }
    return {
      ...common,
      category: /(?:CMakeLists\.txt|\.cmake)$/u.test(file) ? 'core-test-build-input' : 'core-service-test',
      disposition: 'target-contract',
      action: 'migrate-test-then-remove-service-reference',
      targetOwners: [ledgerPath, `${targetSpecRoot}/README.ko.md`],
      removalGate: 'V11-M3-CORE-REMOVE',
    };
  }
  if (file.startsWith('core/doc/')) {
    return {
      ...common,
      category: file.includes('/internals/') ? 'core-internals-reference' : 'core-spec-or-guide-reference',
      disposition: 'superseded',
      action: 'replace-service-document-reference',
      targetOwners: [`${targetSpecRoot}/README.ko.md`, `${targetInternalsRoot}/README.ko.md`],
      removalGate: 'V11-M9-DOCS',
      finalGate: 'V11-R7',
    };
  }
  if (file === 'core/CMakeLists.txt' || file.includes('/builds/')) {
    return {
      ...common,
      category: 'core-build-input',
      disposition: 'raw-core-prerequisite',
      action: 'remove-service-build-reference',
      targetOwners: ['core/doc/internals/runtime-boundary.ko.md'],
      removalGate: 'V11-M3-CORE-CLEAN',
    };
  }
  return {
    ...common,
    category: 'core-mixed-source-reference',
    disposition: file.startsWith('core/src/') ? 'raw-core-prerequisite' : 'superseded',
    action: 'remove-service-reference',
    targetOwners: [
      'core/doc/internals/runtime-boundary.ko.md',
      `${targetInternalsRoot}/README.ko.md`,
    ],
    removalGate: 'V11-M3-CORE-CLEAN',
  };
}

function bindingTargetOwners(language) {
  const definition = bindingDefinitions[language];
  return [definition.exactOwner, definition.additionalOwner].filter(Boolean);
}

function bindingFileRecord(language, file) {
  const definition = bindingDefinitions[language];
  const common = {
    id: `file:${file}`,
    file,
    scope: 'binding',
    language,
    targetOwners: bindingTargetOwners(language),
    removalGate: definition.removalGate,
    finalGate: definition.finalGate,
  };
  if (isBinaryPackage(file)) {
    return {
      ...common,
      category: 'binding-native-package-payload',
      disposition: 'superseded',
      action: 'replace-package-payload',
    };
  }
  if (generatedPathPattern.test(file)) {
    return {
      ...common,
      category: 'binding-generated-reference',
      disposition: 'superseded',
      action: 'replace-generated-output',
    };
  }
  if (isPublicBindingFile(language, file)) {
    return {
      ...common,
      category: 'binding-public-service-projection',
      disposition: 'target-contract',
      action: 'remove-service-projection',
    };
  }
  if (isBindingPublicServiceFactory(file)) {
    return {
      ...common,
      category: 'binding-public-service-factory',
      disposition: 'target-contract',
      action: 'remove-service-projection',
    };
  }
  if (/\/(?:Runtime|runtime)\/(?:Service|service)\//u.test(file)
      || /NativeMethods\.(?:Actor|Dispatch|InstanceSpot|MeshNode|Spot|StreamSession)\.cs$/u.test(file)
      || isBindingNativeServiceProjection(file)
      || /NativeServiceSymbols|ServiceInterop|ServiceLayouts|binding_service|addon_(?:mesh_service|spot)/u.test(file)) {
    return {
      ...common,
      category: 'binding-service-runtime-projection',
      disposition: 'superseded',
      action: 'remove-service-projection',
      targetOwners: [...bindingTargetOwners(language), `${targetInternalsRoot}/README.ko.md`],
    };
  }
  if (zmpHeartbeatMarker.test(readText(file))) {
    return {
      ...common,
      category: file.includes('/tests/') || file.includes('/src/test/')
        ? 'binding-zmp-heartbeat-test'
        : 'binding-zmp-heartbeat-projection',
      disposition: 'remove',
      action: 'remove-zmp-heartbeat-option-projection',
      targetOwners: [
        'core/doc/spec/core/09-runtime-boundary.ko.md',
        `${formalServerSpecRoot}/50-runtime-monitoring.ko.md`,
        `${commonInternalsRoot}/10-liveness-and-state.ko.md`,
      ],
    };
  }
  if (file.includes('/tests/') || file.includes('/src/test/')) {
    return {
      ...common,
      category: 'binding-service-test',
      disposition: 'target-contract',
      action: 'migrate-test-then-remove-service-reference',
      targetOwners: [...bindingTargetOwners(language), ledgerPath],
    };
  }
  if (file.includes('/samples/')) {
    return {
      ...common,
      category: 'binding-service-sample',
      disposition: 'target-contract',
      action: 'migrate-sample-then-remove-service-reference',
      targetOwners: [...bindingTargetOwners(language), ledgerPath],
    };
  }
  if (file.includes('/perf/')) {
    return {
      ...common,
      category: file.includes('/baseline/') ? 'binding-service-perf-archive' : 'binding-service-perf-input',
      disposition: file.includes('/baseline/') ? 'retain-10x-only' : 'superseded',
      action: file.includes('/baseline/') ? 'retain-read-only-archive' : 'remove-after-oracle-baseline-sealed',
      targetOwners: [ledgerPath],
      removalGate: 'V11-M3-PERF-LEGACY',
    };
  }
  if (bindingRootMetadata.has(file)
      || /(?:CMakeLists\.txt|\.cmake|\.csproj|\.sln|build\.gradle|settings\.gradle|package(?:-lock)?\.json|binding\.gyp|tsconfig[^/]*\.json)$/u.test(file)) {
    return {
      ...common,
      category: 'binding-build-or-package-input',
      disposition: 'superseded',
      action: 'remove-service-reference',
    };
  }
  return {
    ...common,
    category: 'binding-mixed-service-reference',
    disposition: 'superseded',
    action: 'remove-service-reference',
  };
}

function legacyFileRecord(language, file) {
  return {
    id: `file:${file}`,
    file,
    scope: 'legacy-binding',
    language,
    category: isBinaryPackage(file) ? 'legacy-native-package-payload' : 'legacy-service-or-package-input',
    disposition: 'retain-10x-only',
    decision: 'Retain/OutOfScopeV11',
    action: 'exclude-from-core-11-build-package-and-compatibility-metadata',
    targetOwners: [ledgerPath, 'scripts/local-package/README.ko.md'],
    removalGate: 'V11-M2-BIND-READINESS',
    finalGate: 'V11-M9-RAW-FINAL',
  };
}

function frameworkFileRecord(language, file) {
  const definition = frameworkDefinitions[language];
  const category = file.includes('/e2e/') || file.includes('/e2e-kotlin/')
    ? 'framework-service-consumer-e2e'
    : file.includes('/tests/') || file.includes('/test/')
      ? 'framework-service-consumer-test'
      : file.includes('/samples/') || file.includes('/sample/')
        ? 'framework-service-consumer-sample'
        : file.endsWith('.md')
          ? 'framework-service-consumer-document'
          : 'framework-service-consumer-source';
  return {
    id: `file:${file}`,
    file,
    scope: 'framework-consumer',
    language,
    category,
    disposition: 'target-contract',
    action: 'retain-framework-owned-service-runtime',
    targetOwners: [
      ...bindingTargetOwners(language),
      `${targetInternalsRoot}/README.ko.md`,
    ],
    removalGate: definition.removalGate,
    finalGate: definition.finalGate,
  };
}

function frameworkAdjacentFileRecord(language, file) {
  return {
    id: `file:${file}`,
    file,
    scope: 'framework-adjacent-component',
    language,
    category: file.includes('http-client') || file.includes('HttpClient')
      ? 'http-client-out-of-scope'
      : 'stream-connector-out-of-scope',
    disposition: 'out-of-scope-v11-service-migration',
    decision: 'Retain/OutOfScopeV11',
    action: 'retain-and-exclude-from-server-framework-service-migration',
    targetOwners: [ledgerPath],
    removalGate: 'V11-M2-BIND-READINESS',
    finalGate: 'V11-R7',
  };
}

function sharedFileRecord(file) {
  if (file.startsWith('framework/runtime/')) {
    return {
      id: `file:${file}`,
      file,
      scope: 'framework-shared-runtime',
      category: 'framework-service-wire-runtime-input',
      disposition: 'target-contract',
      action: 'retain-framework-owned-service-wire-input',
      targetOwners: [
        `${commonInternalsRoot}/service-wire-protocol.ko.md`,
        'framework/doc/framework/common/internals/service-wire-protocol.ko.md',
      ],
      removalGate: 'V11-M5-PROTOCOL',
      finalGate: 'V11-R7',
    };
  }
  if (file.startsWith('bindings/c/perf/')) {
    return {
      id: `file:${file}`,
      file,
      scope: 'shared-perf',
      category: file.includes('/baseline/') ? 'core-10x-perf-archive' : 'active-core-service-perf-input',
      disposition: file.includes('/baseline/') ? 'retain-10x-only' : 'superseded',
      action: file.includes('/baseline/') ? 'retain-read-only-archive' : 'remove-after-oracle-baseline-sealed',
      targetOwners: [ledgerPath],
      removalGate: 'V11-M3-PERF-LEGACY',
      finalGate: 'V11-M9-SMOKE-PERF',
    };
  }
  if (file === 'scripts/local-package/native/sync-local-core-libs.sh') {
    return {
      id: `file:${file}`,
      file,
      scope: 'package-tooling',
      category: 'core-local-package-input',
      disposition: 'raw-core-prerequisite',
      action: 'remove-core-service-header-copy-before-core-11-package',
      targetOwners: [ledgerPath, 'scripts/local-package/README.ko.md'],
      removalGate: 'V11-M3-CORE-CLEAN',
      finalGate: 'V11-M9-RAW-FINAL',
    };
  }
  const bindingPackageLanguage = [
    ['cpp', 'V11-M4-BIND-CPP', 'V11-M4-PKG-CPP'],
    ['dotnet', 'V11-M4-BIND-DN', 'V11-M4-PKG-DN'],
    ['java', 'V11-M4-BIND-JVM', 'V11-M4-PKG-JVM'],
    ['node', 'V11-M4-BIND-NODE', 'V11-M4-PKG-NODE'],
  ].find(([language]) => file.startsWith(`scripts/local-package/${language}/`));
  if (bindingPackageLanguage) {
    const [language, removalGate, finalGate] = bindingPackageLanguage;
    return {
      id: `file:${file}`,
      file,
      scope: 'package-tooling',
      language,
      category: 'binding-local-package-input',
      disposition: 'raw-core-prerequisite',
      action: 'review-binding-package-tooling-before-package-execution',
      targetOwners: [ledgerPath, 'scripts/local-package/README.ko.md'],
      removalGate,
      finalGate,
    };
  }
  if (file.startsWith('scripts/local-package/core/')) {
    return {
      id: `file:${file}`,
      file,
      scope: 'package-tooling',
      category: 'core-local-package-input',
      disposition: 'raw-core-prerequisite',
      action: 'review-core-package-tooling-before-package-execution',
      targetOwners: [ledgerPath, 'scripts/local-package/README.ko.md'],
      removalGate: 'V11-M3-CORE-CLEAN',
      finalGate: 'V11-M3-CORE-PKG',
    };
  }
  if (file.startsWith('scripts/local-package/framework/')) {
    return {
      id: `file:${file}`,
      file,
      scope: 'package-tooling',
      category: 'framework-local-package-input',
      disposition: 'raw-core-prerequisite',
      action: 'review-framework-package-tooling-before-package-execution',
      targetOwners: [ledgerPath, 'scripts/local-package/README.ko.md'],
      removalGate: 'V11-M8-CLEAN-COMMON',
      finalGate: 'V11-R7',
    };
  }
  return {
    id: `file:${file}`,
    file,
    scope: file.startsWith('.github/') ? 'ci' : 'package-tooling',
    category: file.startsWith('.github/') ? 'ci-input' : 'local-package-input',
    disposition: 'raw-core-prerequisite',
    action: 'verify-and-remove-service-package-reference',
    targetOwners: [ledgerPath, 'scripts/local-package/README.ko.md'],
    removalGate: 'V11-M8-CLEAN-COMMON',
    finalGate: 'V11-M9-RAW-FINAL',
  };
}

function historicalFileRecords() {
  return historicalServiceDocuments.map(file => ({
    id: `historical-file:${file}`,
    file,
    scope: 'historical-core-service-document',
    category: file.includes('/internals/') ? 'historical-core-service-internals' : 'historical-core-service-spec',
    presentInTargetTree: fs.existsSync(path.join(repositoryRoot, file)),
    disposition: 'superseded',
    action: 'preserve-meaning-in-target-document-not-file',
    targetOwners: [
      file.includes('/internals/') ? `${targetInternalsRoot}/README.ko.md` : `${targetSpecRoot}/README.ko.md`,
    ],
    removalGate: 'V11-M9-DOCS',
    finalGate: 'V11-R7',
  }));
}

function classificationRoute(file, reader = readText) {
  if (coreCandidate(file, reader)) return 'core';
  const language = languageForFile(file);
  if (language && bindingCandidate(language, file, reader)) return 'binding';
  const frameworkLanguage = frameworkLanguageForFile(file);
  if (frameworkLanguage
      && isFrameworkAdjacentComponent(frameworkLanguage, file)
      && isTextFile(file)
      && serviceMarker.test(reader(file))) return 'framework-adjacent';
  if (frameworkLanguage && frameworkCandidate(frameworkLanguage, file, reader)) {
    return 'framework';
  }
  if (sharedCandidate(file, reader)) return 'shared';
  const legacyLanguage = legacyLanguageForFile(file);
  if (legacyLanguage && legacyCandidate(file, reader)) return 'legacy';
  return undefined;
}

function broadServiceMarkerCandidate(file, reader = readText) {
  if (!isTextFile(file) || buildOutputPattern.test(file) || generatedPathPattern.test(file)) {
    return false;
  }
  const inDiscoveryRoot = file.startsWith('core/')
    || languageForFile(file) !== undefined
    || legacyLanguageForFile(file) !== undefined
    || frameworkLanguageForFile(file) !== undefined
    || file.startsWith('framework/runtime/')
    || file.startsWith('.github/workflows/')
    || file.startsWith('scripts/local-package/');
  if (!inDiscoveryRoot) return false;
  const source = reader(file);
  return source.length > 0 && serviceMarker.test(source);
}

function discoverUnclassifiedServiceFiles(repositoryFiles, reader = readText) {
  return repositoryFiles
    .filter(file => broadServiceMarkerCandidate(file, reader)
      && classificationRoute(file, reader) === undefined)
    .sort((left, right) => left.localeCompare(right, 'en'));
}

const fileRoutingNegativeMutationCount = 10;

function validateInventoryDiscoverySelfTests() {
  const sources = new Map([
    ['bindings/go/__negative_service_route__.go', 'func route() { zlink_mesh_node_open() }'],
    ['.github/workflows/__negative_service_route__.yml', 'run: zlink_mesh_node_open'],
  ]);
  const reader = file => sources.get(file) ?? '';
  const goFile = 'bindings/go/__negative_service_route__.go';
  if (classificationRoute(goFile, reader) !== 'legacy') {
    throw new Error('file-routing negative mutation did not classify a Go service source');
  }
  const workflow = '.github/workflows/__negative_service_route__.yml';
  const unclassified = discoverUnclassifiedServiceFiles([workflow], reader);
  if (!unclassified.includes(workflow)) {
    throw new Error('file-routing negative mutation did not report an unrouted service workflow');
  }

  const structuralCases = [
    {
      shape: 'public context factory',
      file: 'bindings/dotnet/src/Zlink/Contracts/Core/NegativeContext.cs',
      source: 'public interface IContext { IMeshNode CreateMeshNode(); }',
      controlFile: 'bindings/dotnet/src/Zlink/Contracts/Core/RawContext.cs',
      controlSource: 'public interface IContext { IRouterSocket CreateRouterSocket(); }',
    },
    {
      shape: 'native projection',
      file: 'bindings/java/src/main/java/systems/zlink/runtime/native/NativeInstanceSpotModels.java',
      source: 'final class NativeInstanceSpotModels { long placementToken; }',
      controlFile: 'bindings/java/src/main/java/systems/zlink/runtime/native/NativeSocketModels.java',
      controlSource: 'final class NativeSocketModels { long socketToken; }',
    },
    {
      shape: 'service test',
      file: 'bindings/cpp/tests/contract/test_actor_contract.cpp',
      source: 'void verify_actor_contract();',
      controlFile: 'bindings/cpp/tests/contract/test_socket_contract.cpp',
      controlSource: 'void verify_socket_contract();',
    },
    {
      shape: 'service sample',
      file: 'bindings/node/samples/actor_gateway_relay_sample.ts',
      source: 'export function runActorGatewayRelay(): void {}',
      controlFile: 'bindings/node/samples/pair_echo_sample.ts',
      controlSource: 'export function runPairEcho(): void {}',
    },
  ];
  for (const testCase of structuralCases) {
    if (serviceMarker.test(testCase.source)) {
      throw new Error(`structural ${testCase.shape} mutation is not testing the embedded-identifier gap`);
    }
    const structuralReader = file => {
      if (file === testCase.file) return testCase.source;
      if (file === testCase.controlFile) return testCase.controlSource;
      return '';
    };
    if (classificationRoute(testCase.file, structuralReader) !== 'binding') {
      throw new Error(`file-routing negative mutation missed a supported binding ${testCase.shape}`);
    }
    if (classificationRoute(testCase.controlFile, structuralReader) !== undefined) {
      throw new Error(`file-routing negative mutation classified an unrelated raw ${testCase.shape}`);
    }
  }

  const header = 'core/include/zlink/service/__negative_public_symbols.h';
  const baseline = [
    '#define ZLINK_SPOT_NEGATIVE_LIMIT 7',
    'typedef enum zlink_spot_negative_mode_t {',
    '  ZLINK_SPOT_NEGATIVE_OFF = 0,',
    '  ZLINK_SPOT_NEGATIVE_ON = 1',
    '} zlink_spot_negative_mode_t;',
    'typedef struct zlink_spot_negative_t {',
    '  uint32_t count;',
    '  void (*notify)(void *);',
    '} zlink_spot_negative_t;',
    'typedef void (*zlink_spot_negative_callback_fn)(void *userdata);',
    'ZLINK_EXPORT int zlink_spot_negative_open(const zlink_spot_negative_t *value);',
  ].join('\n');
  const symbols = publicHeaderSymbols(header, true, () => baseline);
  for (const identity of [
    'macro::ZLINK_SPOT_NEGATIVE_LIMIT',
    'enum-type::zlink_spot_negative_mode_t',
    'enum-value:zlink_spot_negative_mode_t:ZLINK_SPOT_NEGATIVE_ON',
    'struct-type::zlink_spot_negative_t',
    'struct-field:zlink_spot_negative_t:notify',
    'callback::zlink_spot_negative_callback_fn',
    'function::zlink_spot_negative_open',
  ]) {
    const actual = symbols.some(symbol => `${symbol.kind}:${symbol.parent ?? ''}:${symbol.symbol}` === identity);
    if (!actual) throw new Error(`public-header self-test missed representative symbol: ${identity}`);
  }
  const mutated = `${baseline}\nZLINK_EXPORT int zlink_spot_negative_close(void);`;
  const mutatedSymbols = publicHeaderSymbols(header, true, () => mutated);
  if (!mutatedSymbols.some(symbol => symbol.kind === 'function'
      && symbol.symbol === 'zlink_spot_negative_close')
      || mutatedSymbols.length !== symbols.length + 1) {
    throw new Error('public-header negative mutation did not detect an added C function');
  }
}

function fileRecords(repositoryFiles, reviewedCoreRemovalFiles) {
  validateCoreServiceDocumentationReviews(repositoryFiles);
  validateCompletedCoreRawSpecRewrites();
  validateInventoryDiscoverySelfTests();
  const unclassified = discoverUnclassifiedServiceFiles(repositoryFiles);
  if (unclassified.length > 0) {
    throw new Error(`unclassified service-marker files (${unclassified.length}):\n  ${unclassified.join('\n  ')}`);
  }
  const records = [];
  const seen = new Set();
  for (const file of repositoryFiles) {
    let record;
    const route = classificationRoute(file);
    if (route === 'core') record = coreFileRecord(file);
    else {
      const language = languageForFile(file);
      if (route === 'binding') record = bindingFileRecord(language, file);
      else {
        const frameworkLanguage = frameworkLanguageForFile(file);
        if (route === 'framework-adjacent') {
          record = frameworkAdjacentFileRecord(frameworkLanguage, file);
        } else if (route === 'framework') {
          record = frameworkFileRecord(frameworkLanguage, file);
        } else {
          const legacyLanguage = legacyLanguageForFile(file);
          if (route === 'shared') record = sharedFileRecord(file);
          else if (route === 'legacy') record = legacyFileRecord(legacyLanguage, file);
        }
      }
    }
    if (!record) continue;
    if (seen.has(record.id)) throw new Error(`duplicate file classification: ${file}`);
    seen.add(record.id);
    records.push(record);
  }
  for (const [language, definition] of Object.entries(frameworkDefinitions)) {
    if (!repositoryFiles.includes(definition.auditFile)) {
      throw new Error(`Framework removal audit anchor is missing: ${definition.auditFile}`);
    }
    const record = {
      id: `framework-removal-audit:${language}`,
      file: definition.auditFile,
      scope: 'framework-consumer',
      language,
      category: 'framework-binding-service-projection-removal-audit',
      disposition: 'remove',
      action: 'remove-framework-binding-service-projection-reference',
      targetOwners: [
        ...bindingTargetOwners(language),
        `${commonInternalsRoot}/README.ko.md`,
      ],
      removalGate: language === 'java' ? 'V11-M5-SCAFFOLD-JVM'
        : language === 'dotnet' ? 'V11-M5-SCAFFOLD-DN'
          : `V11-M5-SCAFFOLD-${language.toUpperCase()}`,
      finalGate: definition.finalGate,
    };
    if (seen.has(record.id)) throw new Error(`duplicate Framework audit classification: ${record.id}`);
    seen.add(record.id);
    records.push(record);
  }
  for (const record of historicalFileRecords()) {
    if (seen.has(record.id)) throw new Error(`duplicate historical classification: ${record.file}`);
    seen.add(record.id);
    records.push(record);
  }
  const byId = new Map(records.map(record => [record.id, record]));
  for (const record of reviewedCoreRemovalFiles) {
    const current = byId.get(record.id);
    if (current && recordIdentity(current) !== recordIdentity(record)) {
      throw new Error(`reviewed Core removal-file classification changed: ${record.id}`);
    }
    if (!current) {
      records.push(record);
      seen.add(record.id);
    }
  }
  return records.sort((left, right) => left.id.localeCompare(right.id, 'en'));
}

function extractEnumMembers(source, language) {
  const members = [];
  const patterns = language === 'cpp'
    ? [/enum\s+class\s+([A-Za-z_]\w*)[^\{]*\{([\s\S]*?)\}\s*;/gu]
    : language === 'dotnet'
      ? [/public\s+(?:\[[^\]]*\]\s*)*enum\s+([A-Za-z_]\w*)[^\{]*\{([\s\S]*?)\}/gu]
      : language === 'java'
        ? [/public\s+enum\s+([A-Za-z_]\w*)[^\{]*\{([\s\S]*?)\}/gu]
        : [];
  for (const pattern of patterns) {
    for (const match of source.matchAll(pattern)) {
      for (const value of splitCommaList(match[2])) {
        const name = normalizeSpace(value).match(/^([A-Za-z_]\w*)/u)?.[1];
        if (name) members.push({kind: 'enum-value', symbol: name, parent: match[1]});
      }
    }
  }
  return members;
}

function projectionDeclarations(language, file) {
  const source = stripComments(readText(file));
  const declarations = [];
  if (language === 'cpp') {
    if (/\/include\/zlink\/service\/[^/]+\.h$/u.test(file)) {
      for (const symbol of publicHeaderSymbols(file, true)) {
        declarations.push({
          kind: `c-${symbol.kind}`,
          symbol: symbol.symbol,
          parent: symbol.parent,
          signature: symbol.signature ?? symbol.declaration ?? symbol.value ?? '',
        });
      }
    }
    for (const match of source.matchAll(/\b(enum\s+class|class|struct)\s+([A-Za-z_]\w*)/gu)) {
      declarations.push({kind: match[1].replace(/\s+/gu, '-'), symbol: match[2]});
    }
    for (const match of source.matchAll(/\busing\s+([A-Za-z_]\w*)\s*=/gu)) {
      declarations.push({kind: 'type-alias', symbol: match[1]});
    }
  } else if (language === 'dotnet') {
    for (const match of source.matchAll(/\bpublic\s+(?:(?:sealed|readonly|partial|static|abstract)\s+)*(class|struct|interface|enum|record(?:\s+struct)?|delegate)\s+([A-Za-z_]\w*)/gu)) {
      declarations.push({kind: match[1].replace(/\s+/gu, '-'), symbol: match[2]});
    }
  } else if (language === 'java') {
    for (const match of source.matchAll(/\bpublic\s+(?:(?:sealed|non-sealed|final|abstract|static)\s+)*(class|interface|enum|record)\s+([A-Za-z_]\w*)/gu)) {
      declarations.push({kind: match[1], symbol: match[2]});
    }
  } else if (language === 'node') {
    for (const match of source.matchAll(/\bexport\s+(?:declare\s+)?(?:abstract\s+)?(interface|class|enum|type|function|const)\s+([A-Za-z_$][\w$]*)/gu)) {
      declarations.push({kind: match[1], symbol: match[2]});
    }
    for (const match of source.matchAll(/\bexport\s*\{([^}]*)\}/gu)) {
      for (const item of splitCommaList(match[1])) {
        const value = normalizeSpace(item);
        const name = value.match(/^(?:type\s+)?([A-Za-z_$][\w$]*)/u)?.[1];
        if (name) declarations.push({kind: 're-export', symbol: name});
      }
    }
  }
  declarations.push(...extractEnumMembers(source, language));
  const unique = new Map();
  for (const declaration of declarations) {
    const key = `${declaration.kind}:${declaration.parent ?? ''}:${declaration.symbol}`;
    unique.set(key, declaration);
  }
  return [...unique.values()].sort((left, right) => {
    const leftKey = `${left.kind}:${left.parent ?? ''}:${left.symbol}`;
    const rightKey = `${right.kind}:${right.parent ?? ''}:${right.symbol}`;
    return leftKey.localeCompare(rightKey, 'en');
  });
}

function bindingProjectionDeclarationRecords(files) {
  const records = [];
  for (const [language, definition] of Object.entries(bindingDefinitions)) {
    const publicFiles = files
      .filter(record => record.scope === 'binding'
        && record.language === language
        && isPublicBindingFile(language, record.file))
      .map(record => record.file);
    for (const file of publicFiles) {
      for (const declaration of projectionDeclarations(language, file)) {
        records.push({
          id: `binding-public-declaration:${language}:${file}:${declaration.kind}:${declaration.parent ?? ''}:${declaration.symbol}`,
          language,
          file,
          ...declaration,
          disposition: 'target-contract',
          action: 'remove-binding-service-declaration-after-framework-runtime-replacement',
          targetOwners: bindingTargetOwners(language),
          removalGate: definition.removalGate,
          finalGate: definition.finalGate,
        });
      }
    }
  }
  return records.sort((left, right) => left.id.localeCompare(right.id, 'en'));
}

function bindingProjectionUnitRecords(files) {
  return files
    .filter(record => record.scope === 'binding'
      && isPublicBindingFile(record.language, record.file))
    .map(record => {
      const definition = bindingDefinitions[record.language];
      return {
        id: `binding-public-semantic-unit:${record.language}:${record.file}`,
        language: record.language,
        file: record.file,
        kind: 'comment-free-normalized-source',
        semanticSource: normalizeSpace(stripComments(readText(record.file))),
        disposition: 'target-contract',
        action: 'remove-binding-service-projection-after-framework-runtime-replacement',
        targetOwners: bindingTargetOwners(record.language),
        removalGate: definition.removalGate,
        finalGate: definition.finalGate,
      };
    })
    .sort((left, right) => left.id.localeCompare(right.id, 'en'));
}

function containsIdentifier(source, identifier) {
  const escaped = identifier.replace(/[.*+?^${}()|[\]\\]/gu, '\\$&');
  return new RegExp(`(?<![A-Za-z0-9_])${escaped}(?![A-Za-z0-9_])`, 'u').test(source);
}

function bindingCoreReferenceRecords(files, coreSymbols) {
  const records = [];
  const uniqueSymbols = new Map();
  for (const record of coreSymbols) {
    if (record.kind.endsWith('-field')) continue;
    if (!uniqueSymbols.has(record.symbol)) uniqueSymbols.set(record.symbol, record);
  }
  for (const fileRecord of files) {
    if (fileRecord.scope !== 'binding') continue;
    const source = readText(fileRecord.file);
    if (!source) continue;
    const definition = bindingDefinitions[fileRecord.language];
    for (const [symbol, coreRecord] of uniqueSymbols) {
      if (!containsIdentifier(source, symbol)) continue;
      records.push({
        id: `binding-core-symbol-reference:${fileRecord.language}:${fileRecord.file}:${symbol}`,
        language: fileRecord.language,
        file: fileRecord.file,
        kind: 'core-symbol-reference',
        coreSymbol: symbol,
        coreSymbolKind: coreRecord.kind,
        disposition: coreRecord.disposition === 'target-contract' ? 'target-contract' : 'superseded',
        action: 'remove-binding-service-projection',
        targetOwners: bindingTargetOwners(fileRecord.language),
        removalGate: definition.removalGate,
        finalGate: definition.finalGate,
      });
    }
  }
  return records.sort((left, right) => left.id.localeCompare(right.id, 'en'));
}

function countBy(records, selector) {
  const counts = {};
  for (const record of records) {
    const key = selector(record) ?? 'none';
    counts[key] = (counts[key] ?? 0) + 1;
  }
  return Object.fromEntries(Object.entries(counts).sort(([left], [right]) => left.localeCompare(right, 'en')));
}

function validateRecordSet(records, section) {
  const ids = new Set();
  for (const record of records) {
    if (!record.id || ids.has(record.id)) throw new Error(`${section} has duplicate or empty id: ${record.id}`);
    ids.add(record.id);
    if (!allowedDispositions.has(record.disposition)) {
      throw new Error(`${section} ${record.id} has unsupported disposition: ${record.disposition}`);
    }
    if (!record.action || !record.removalGate || !record.finalGate) {
      throw new Error(`${section} ${record.id} is missing action or gate`);
    }
    if (!actionDefinitions[record.action]) {
      throw new Error(`${section} ${record.id} has no action scope definition: ${record.action}`);
    }
    if (!Array.isArray(record.targetOwners) || record.targetOwners.length === 0) {
      throw new Error(`${section} ${record.id} has no target owner`);
    }
    for (const owner of record.targetOwners) {
      if (!fs.existsSync(path.join(repositoryRoot, owner))) {
        throw new Error(`${section} ${record.id} points to a missing target owner: ${owner}`);
      }
    }
  }
}

function generateInventory() {
  const repositoryFiles = readRepositoryFiles();
  const reviewedCoreBaseline = loadReviewedCoreBaseline();
  const reviewedCoreRemovalFiles = loadReviewedCoreRemovalFiles();
  const currentCorePublicSymbols = discoverCurrentCorePublicSymbolRecords();
  const currentCoreExportSymbols = exportedServiceSymbolRecords(currentCorePublicSymbols);
  validateCoreSurfaceStateSelfTests(reviewedCoreBaseline.corePublicSymbols);
  validateCoreSurfaceStateSelfTests(reviewedCoreBaseline.coreExportSymbols);
  const reviewedServiceHeaderSymbols = reviewedCoreBaseline.corePublicSymbols
    .filter(record => coreServiceHeaders.includes(record.file));
  const currentServiceHeaderSymbols = currentCorePublicSymbols
    .filter(record => coreServiceHeaders.includes(record.file));
  const serviceHeaderSymbolState = validateCoreSurfaceState(
    reviewedServiceHeaderSymbols,
    currentServiceHeaderSymbols,
    'current Core service-header symbol surface');
  // These records are service fragments formerly declared in otherwise raw
  // support headers (mesh monitor, channel name, heartbeat options, and the
  // mesh poller source).  The support headers themselves remain mandatory.
  const reviewedSupportHeaderServiceSymbols = reviewedCoreBaseline.corePublicSymbols
    .filter(record => coreSupportHeaders.includes(record.file));
  const currentSupportHeaderServiceSymbols = currentCorePublicSymbols
    .filter(record => coreSupportHeaders.includes(record.file));
  const supportHeaderServiceSymbolState = validateCoreSurfaceState(
    reviewedSupportHeaderServiceSymbols,
    currentSupportHeaderServiceSymbols,
    'current service fragments in Core raw support headers');
  const exportSurfaceState = validateCoreSurfaceState(
    reviewedCoreBaseline.coreExportSymbols,
    currentCoreExportSymbols,
    'current Core service export surface');
  const presentServiceHeaders = coreServiceHeaders.filter(file =>
    fs.existsSync(path.join(repositoryRoot, file)));
  const serviceHeaderState = presentServiceHeaders.length === coreServiceHeaders.length
    ? 'reviewed-baseline-present'
    : presentServiceHeaders.length === 0
      ? 'removed'
      : 'partial-removal';
  if (serviceHeaderState === 'partial-removal') {
    throw new Error(`Core service headers are partially removed: ${presentServiceHeaders.join(', ')}`);
  }
  if (new Set([
    serviceHeaderSymbolState,
    supportHeaderServiceSymbolState,
    exportSurfaceState,
    serviceHeaderState,
  ]).size !== 1) {
    throw new Error([
      'Core service headers, declarations, and exports are in inconsistent transition states',
      `headers=${serviceHeaderState}`,
      `serviceHeaderSymbols=${serviceHeaderSymbolState}`,
      `supportHeaderServiceSymbols=${supportHeaderServiceSymbolState}`,
      `exports=${exportSurfaceState}`,
    ].join('\n'));
  }
  const corePublicSymbols = reviewedCoreBaseline.corePublicSymbols;
  const coreExportSymbols = reviewedCoreBaseline.coreExportSymbols;
  const files = fileRecords(repositoryFiles, reviewedCoreRemovalFiles);
  const bindingPublicDeclarations = bindingProjectionDeclarationRecords(files);
  const bindingProjectionUnits = bindingProjectionUnitRecords(files);
  const bindingCoreSymbolReferences = bindingCoreReferenceRecords(files, corePublicSymbols);

  validateRecordSet(corePublicSymbols, 'corePublicSymbols');
  validateRecordSet(coreExportSymbols, 'coreExportSymbols');
  validateRecordSet(bindingPublicDeclarations, 'bindingPublicDeclarations');
  validateRecordSet(bindingProjectionUnits, 'bindingProjectionUnits');
  validateRecordSet(bindingCoreSymbolReferences, 'bindingCoreSymbolReferences');
  validateRecordSet(files, 'files');

  const allIds = new Set();
  for (const records of [corePublicSymbols, coreExportSymbols, bindingPublicDeclarations,
    bindingProjectionUnits,
    bindingCoreSymbolReferences, files]) {
    for (const record of records) {
      if (allIds.has(record.id)) throw new Error(`inventory sections reuse id: ${record.id}`);
      allIds.add(record.id);
    }
  }

  return {
    schema: 1,
    version: '11.0.0',
    description: 'Reviewed inventory for removing Core service ABI and replacing the four supported binding projections with per-language Framework runtimes.',
    comparisonPolicy: {
      semanticIdsOnly: true,
      fileHashes: false,
      timestamps: false,
      proseChangesBlock: false,
      newlyDiscoveredSymbolsOrFilesBlock: true,
      note: 'Regenerate after reviewing a semantic addition or removal. Documentation prose, source comments, whitespace, file hashes, and timestamps are not compared. A supported binding public service projection is compared as normalized comment-free source so member-level changes cannot bypass review.',
    },
    scope: {
      coreServiceHeaders,
      coreSupportHeaders,
      reviewedCoreBaseline: {
        source: relativePath(inventoryPath),
        sections: ['corePublicSymbols', 'coreExportSymbols'],
        sha256: reviewedCoreBaselineSha256,
        policy: 'The sealed reviewed records preserve Core 10.x removal provenance after the source headers and exports are deleted. The current tree may match this baseline or contain zero Core service surface; partial transitions and mutations fail generation.',
      },
      reviewedCoreRemovalFiles: {
        sourceRevision: reviewedCoreRemovalFilesRevision,
        source: relativePath(inventoryPath),
        selection: 'scope=core and removalGate in {V11-M3-CORE-REMOVE,V11-M3-CORE-CLEAN}',
        records: reviewedCoreRemovalFiles.length,
        sha256: reviewedCoreRemovalFilesSha256,
        policy: 'The sealed records remain in files after deletion so every removed Core service file keeps its reviewed disposition, action, owners, and gates.',
      },
      currentCoreServiceSurface: {
        state: serviceHeaderSymbolState,
        serviceHeadersPresent: presentServiceHeaders.length,
        serviceHeaderSymbolsPresent: currentServiceHeaderSymbols.length,
        supportHeadersPresent: coreSupportHeaders.length,
        supportHeaderServiceSymbolsPresent: currentSupportHeaderServiceSymbols.length,
        exportsPresent: currentCoreExportSymbols.length,
      },
      bindingLanguages: Object.keys(bindingDefinitions),
      frameworkConsumerLanguages: Object.keys(frameworkDefinitions),
      legacyBindings: Object.keys(legacyBindingRoots),
      bindingProjectionGranularity: [
        'top-level public types and enum values are independent records',
        'each public service projection file has a comment-free normalized semantic unit, so any member-level source change requires classification review',
        'every exact Core service symbol reference in a supported binding file is an independent record',
      ],
      bindingStructuralDiscovery: [
        'a supported binding public context or factory is included when it declares a service factory member',
        'a supported binding native projection is included when its path or comment-free source contains a service identifier',
        'a supported binding test or sample is included when its path or comment-free source contains a service identifier',
        'an unrelated raw file in the same structural location is not included without a service identifier',
      ],
      coreServiceDocumentationReviews: [...coreServiceDocumentationReviews.values()]
        .sort((left, right) => left.file.localeCompare(right.file, 'en')),
      coreServiceDocumentationNegativeMutations: 2,
      coreFormalRawRewriteNegativeMutations: 1,
      fileRoutingNegativeMutations: fileRoutingNegativeMutationCount,
      corePublicHeaderNegativeMutations: 2,
      coreRemovalStateNegativeMutations: 4,
      broadServiceMarkerDiscovery: 'Known Core, binding, Framework runtime, CI, and local-package roots are scanned. A marked file without a routing classification is a generation error.',
      targetSpec: `${targetSpecRoot}/README.ko.md`,
      targetInternals: `${targetInternalsRoot}/README.ko.md`,
      ledger: ledgerPath,
    },
    dispositions: {
      'target-contract': 'Application-visible meaning moves to Framework spec, exact interfaces, runtime tests, or E2E before the Core or binding projection is removed.',
      'raw-core-prerequisite': 'The file or capability remains part of the Core 11 raw runtime or the package tooling that proves that raw boundary.',
      superseded: 'The service-specific mechanism is replaced by language runtime internals and must not become a Framework public API.',
      remove: 'The classified unit has no Core 11 contract owner. Consult actionDefinitions to determine whether the unit is a whole file, a symbol, or only a fragment in a mixed file.',
      'retain-10x-only': 'The item remains only in the last supported Core 10.x line or read-only archive and is excluded from Core 11 packages and compatibility claims.',
      'out-of-scope-v11-service-migration': 'The item belongs to an independent Framework package such as HTTP client or stream connector and is retained outside the server service-runtime migration lanes.',
    },
    actionDefinitions,
    summary: {
      totalRecords: corePublicSymbols.length + coreExportSymbols.length
        + bindingPublicDeclarations.length + bindingProjectionUnits.length
        + bindingCoreSymbolReferences.length + files.length,
      corePublicSymbols: corePublicSymbols.length,
      corePublicSymbolsByKind: countBy(corePublicSymbols, record => record.kind),
      corePublicSymbolsByDisposition: countBy(corePublicSymbols, record => record.disposition),
      coreExportSymbols: coreExportSymbols.length,
      bindingPublicDeclarations: bindingPublicDeclarations.length,
      bindingPublicDeclarationsByLanguage: countBy(bindingPublicDeclarations, record => record.language),
      bindingProjectionUnits: bindingProjectionUnits.length,
      bindingProjectionUnitsByLanguage: countBy(bindingProjectionUnits, record => record.language),
      bindingCoreSymbolReferences: bindingCoreSymbolReferences.length,
      bindingCoreSymbolReferencesByLanguage: countBy(bindingCoreSymbolReferences, record => record.language),
      files: files.length,
      filesByScope: countBy(files, record => record.scope),
      filesByDisposition: countBy(files, record => record.disposition),
      coreServiceDocumentationReviews: coreServiceDocumentationReviews.size,
      coreServiceDocumentationReviewsByDecision: countBy(
        [...coreServiceDocumentationReviews.values()], record => record.reviewDecision),
      coreServiceDocumentationNegativeMutations: 2,
      coreFormalRawRewriteNegativeMutations: 1,
      fileRoutingNegativeMutations: fileRoutingNegativeMutationCount,
      corePublicHeaderNegativeMutations: 2,
      coreRemovalStateNegativeMutations: 4,
      unclassifiedServiceFiles: 0,
    },
    corePublicSymbols,
    coreExportSymbols,
    bindingPublicDeclarations,
    bindingProjectionUnits,
    bindingCoreSymbolReferences,
    files,
  };
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function flattenInventory(inventory) {
  const result = new Map();
  for (const section of ['corePublicSymbols', 'coreExportSymbols', 'bindingPublicDeclarations',
    'bindingProjectionUnits',
    'bindingCoreSymbolReferences', 'files']) {
    for (const record of inventory[section] ?? []) result.set(record.id, {section, record});
  }
  return result;
}

function checkInventory(expected) {
  if (!fs.existsSync(inventoryPath)) {
    throw new Error(`inventory is missing: ${relativePath(inventoryPath)}; run with --write`);
  }
  let actual;
  try {
    actual = JSON.parse(fs.readFileSync(inventoryPath, 'utf8'));
  } catch (error) {
    throw new Error(`inventory is not valid JSON: ${error.message}`);
  }
  const expectedFlat = flattenInventory(expected);
  const actualFlat = flattenInventory(actual);
  const added = [...expectedFlat.keys()].filter(id => !actualFlat.has(id));
  const removed = [...actualFlat.keys()].filter(id => !expectedFlat.has(id));
  const changed = [...expectedFlat.keys()].filter(id => actualFlat.has(id)
    && JSON.stringify(expectedFlat.get(id)) !== JSON.stringify(actualFlat.get(id)));
  const metadataExpected = {...expected};
  const metadataActual = {...actual};
  for (const section of ['corePublicSymbols', 'coreExportSymbols', 'bindingPublicDeclarations',
    'bindingProjectionUnits',
    'bindingCoreSymbolReferences', 'files']) {
    delete metadataExpected[section];
    delete metadataActual[section];
  }
  const metadataChanged = JSON.stringify(metadataExpected) !== JSON.stringify(metadataActual);
  if (added.length || removed.length || changed.length || metadataChanged) {
    const lines = ['v11 Core service migration inventory differs from the reviewed manifest.'];
    if (added.length) lines.push(`newly discovered and not yet reviewed (${added.length}):\n  ${added.slice(0, 30).join('\n  ')}`);
    if (removed.length) lines.push(`no longer discovered (${removed.length}):\n  ${removed.slice(0, 30).join('\n  ')}`);
    if (changed.length) lines.push(`classification changed (${changed.length}):\n  ${changed.slice(0, 30).join('\n  ')}`);
    if (metadataChanged) lines.push('summary or inventory policy differs from generated semantic records');
    lines.push('Review the semantic change, then run this script with --write. File hashes and prose-only edits are not compared.');
    throw new Error(lines.join('\n'));
  }
}

function printSummary(inventory, prefix) {
  const summary = inventory.summary;
  console.log(`${prefix}: total=${summary.totalRecords}`);
  console.log(`  core public symbols=${summary.corePublicSymbols}, exports=${summary.coreExportSymbols}`);
  console.log(`  binding declarations=${summary.bindingPublicDeclarations} ${JSON.stringify(summary.bindingPublicDeclarationsByLanguage)}`);
  console.log(`  binding semantic units=${summary.bindingProjectionUnits} ${JSON.stringify(summary.bindingProjectionUnitsByLanguage)}`);
  console.log(`  binding Core-symbol references=${summary.bindingCoreSymbolReferences} ${JSON.stringify(summary.bindingCoreSymbolReferencesByLanguage)}`);
  console.log(`  files=${summary.files} ${JSON.stringify(summary.filesByScope)}`);
  console.log(`  Core document reviews=${summary.coreServiceDocumentationReviews} ${JSON.stringify(summary.coreServiceDocumentationReviewsByDecision)}; document-review negative mutations=${summary.coreServiceDocumentationNegativeMutations}; raw-rewrite negative mutations=${summary.coreFormalRawRewriteNegativeMutations}; file-routing negative mutations=${summary.fileRoutingNegativeMutations}; public-header negative mutations=${summary.corePublicHeaderNegativeMutations}`);
}

function usage() {
  console.error('usage: generate-v11-core-service-migration-inventory.mjs --write|--check');
}

const mode = process.argv[2];
if (mode !== '--write' && mode !== '--check') {
  usage();
  process.exitCode = 2;
} else {
  try {
    const inventory = generateInventory();
    if (mode === '--write') {
      fs.mkdirSync(path.dirname(inventoryPath), {recursive: true});
      fs.writeFileSync(inventoryPath, stableJson(inventory));
      printSummary(inventory, `wrote ${relativePath(inventoryPath)}`);
    } else {
      checkInventory(inventory);
      printSummary(inventory, `verified ${relativePath(inventoryPath)}`);
    }
  } catch (error) {
    console.error(error.stack ?? error.message);
    process.exitCode = 1;
  }
}
