// SPDX-License-Identifier: MPL-2.0

const normalize = value => value.toLowerCase()
  .replace(/[^a-z0-9]+/gu, '');

export const semanticMemberKey = member => [
  member.language,
  normalize(member.ownerIdentity),
  normalize(member.memberName),
  member.kind,
].join('\0');

const portableOwner = ownerIdentity => {
  const simple = ownerIdentity.split(/::|\./u).at(-1) ?? ownerIdentity;
  const normalized = normalize(simple).replace(/^izlink|^zlink/gu, '');
  return /_t$/u.test(simple) ? normalized.slice(0, -1) : normalized;
};

export const removedMemberParityKey = member => {
  const owner = portableOwner(member.ownerIdentity);
  const name = normalize(member.memberName);
  if (member.language === 'kotlin' && /package|extensionskt/u.test(owner)) {
    return `kotlin-logical.${name}`;
  }
  if (/entryspotoptions/u.test(owner) && /^(?:set)?routingid$/u.test(name)) {
    return 'entry-spot-options.routing-id';
  }
  if (/meshnodebuilder/u.test(owner) && name === 'setentryspotroutingid') {
    return 'entry-spot-options.routing-id';
  }
  if (/instancespotfactoryoptions/u.test(owner) && name === 'maxactiveinstances') {
    return 'instance-spot-factory-options.max-active-instances';
  }
  if (/instancespotfactoryoptions/u.test(owner) && /activationtimeout(?:ms)?/u.test(name)) {
    return 'instance-spot-factory-options.activation-timeout';
  }
  if (/meshnodebuilder/u.test(owner) && /^(?:add)?actorfactory$/u.test(name)) {
    return 'mesh-node-builder.actor-factory-registration';
  }
  if (/meshNodeDescriptor/i.test(owner) && name === 'spottypes') {
    return 'mesh-node-descriptor.spot-types';
  }
  if (/objectcapability/u.test(owner) && name === 'available') {
    return 'object-capability.available';
  }
  return `${owner}.${name}.${member.kind}`;
};

export const replacementParitySignature = behavior => JSON.stringify({
  decisions: [...behavior.decisionCoverage].sort(),
  coverage: behavior.replacementCoverage
    .map(id => id.replace(/:(?:cpp|dotnet|java|node)$/u, ':<runtime>'))
    .sort(),
});

const runtimeLanguage = language => language === 'kotlin' ? 'java' : language;

const normalizedOwner = member => normalize(member.ownerIdentity);
const normalizedMemberName = member => normalize(member.memberName);
const isKotlinSourcePackageMember = member => member.language === 'kotlin'
  && member.ownerIdentity.includes('::<package>');

export const closedCatchAllExpectations = {
  'host-lifecycle-topology-state-separation': {
    count: 67,
    identitySetSha256: 'bb3863b31e09ed67900adcc696533d830e91d51cd7ba1a9f1a87a62b1cc82075',
  },
  'public-boundary-internal-surface-cleanup': {
    count: 1635,
    identitySetSha256: '3fea348cb01f6b3deda0d19a3bc34ea6bd298aa962a484552e101e06cd5c5513',
  },
  'kotlin-reviewed-contract-set': {
    count: 126,
    identitySetSha256: 'cb73a1e0bc3970a015f6c1e92e6a058af1e06556af85193be0b346d56642ae6b',
  },
  'node-reviewed-contract-set': {
    count: 58,
    identitySetSha256: 'e63caa4c9bf3c253bbff69bc5db76c3871f1a9e9e1c85f370cd59fd2c5b04c4e',
  },
};

export const sourceJvmParityExpectation = {
  groups: 48,
  recoveredPairs: 48,
  identitySetSha256: '2dc995b16bbea555d030fe1643b534134213642205e98bc203d94aa5393f0dcc',
};

// These runners were reviewed after their public-contract call sites changed.
// Pin exact hashes so a later runner edit cannot silently refresh the quarantine baseline.
export const reviewedRegistrationHashes = {
  'framework/languages/cpp/e2e/ObservabilityOps/run_e2e.sh': '8998d293234afcaad7a4d4d84cdbb3003fe41e6754258df98b572237400f88c6',
  'framework/languages/cpp/e2e/RuntimeMonitoring/run_e2e.sh': '33d051a3d372eeaf249e5ffd3549c2311390c1f7aecff1b3214d331ae98328cc',
  'framework/languages/cpp/e2e/SpotService/run_e2e.sh': 'd5abc973d1d9da1ffefe68cd80ad07bb8d913e73a3ebfbbf97b44d1c939e7cfb',
  'framework/languages/cpp/e2e/SubmitAdmission/run_e2e.sh': 'fece0f5772d41f6f7b772953f7902e6a3c841668449ce855e38a87302e509bf1',
  'framework/languages/dotnet/e2e/LocationMessaging/run_e2e.sh': '99e861afdade3eb919214114f12b1ab0b51dfb1d0eb5545deeac50ff572e5178',
  'framework/languages/dotnet/e2e/RuntimeMonitoring/Client/RuntimeMonitoring.Client.csproj': '4f563f907d7d4f41293ee5728ebc376e757677f4063be4ec200ee42b7e044e46',
  'framework/languages/dotnet/e2e/RuntimeMonitoring/run_e2e.sh': '0ba055183ce22a935cc3fc59d0d4f7894d9ac5797e4bbf3e55c730d45249aa8a',
  'framework/languages/dotnet/e2e/SpotActorTransfer/run_e2e.sh': '436ba2af7ea90323f59d266fd14bf0fd69ba7f7ee1ae980ed37251347fd025a7',
  'framework/languages/dotnet/e2e/SpotService/run_e2e.sh': '5d7022adb65acfbe084d05bb4d2e5dd15827bd7764ae80e8e68e5e82eb4c10e1',
  'framework/languages/java/e2e/ObservabilityOps/run_e2e.sh': '1e466b927242ecd761c5ef81b8d4e3de9370cb88b4f9d53db2f3bea69b0fcf72',
  'framework/languages/java/e2e/RuntimeMonitoring/Client/build.gradle.kts': '0ef1a89959b0570b7a71dda85fae82de50d3ab6a34a9b750b1bf73ea2b17338f',
  'framework/languages/java/e2e/RuntimeMonitoring/run_e2e.sh': '4754c3a62f32543caddeefb1ecdb31a48b4e882dd508120184d3b76f64c65d48',
  'framework/languages/java/e2e/SpotActorTransfer/run_e2e.sh': '0028445c9631926b4deb8c773289592e3b5fad2b4fa40dd651a08d6a1b6d2b6c',
  'framework/languages/java/e2e-kotlin/ObservabilityOps/run_e2e.sh': '008e1562134fa4b7424e9a1bcd3ec84aeae123b083a81b29b8f6ef354260ba3a',
  'framework/languages/node/e2e/SpotService/run_e2e.sh': '8217f26dfc6c8e6647381ea68738ecd5e0f7d4c6493dea603a506a1904d15f37',
  'framework/languages/node/e2e/SubmitAdmission/run_e2e.sh': '6e282d43354e1237edb99b888d1ba2405393c0f5373bff3a2ac3fa6f0d8d7624',
  'framework/languages/cpp/samples/Bingo/run_sample.sh': 'e4878971773cce8228318f4afad8d7249bb11a5942ec01af283a697458d121b0',
  'framework/languages/cpp/samples/DeliveryDispatch/run_sample.sh': 'e4524f8c6277bd01dd94458d9403488d86697c3a2e3a223f97ef4282acee343b',
  'framework/languages/cpp/samples/TicTacToe/run_sample.ps1': 'a876209333e27d768e9f3f54a631f44dea5a7e22e3544b53247d81f22ecd1830',
  'framework/languages/cpp/samples/TicTacToe/run_sample.sh': '4cfa92043fa93ecf3d9bf798dcd93748827cb8fb5e9093c13d2009fa79b8dc5c',
  'framework/languages/dotnet/samples/DeliveryDispatch/run_sample.ps1': '0d54f36961f0dac4aca1f8f24bb60b6f8d606f9d202014a47efeebd993997afe',
  'framework/languages/dotnet/samples/DeliveryDispatch/run_sample.sh': 'baba59a6df06b3b00efa412eed8b3c7e7fc0ed4e1967352be9f830586d2148b8',
  'framework/languages/dotnet/samples/ShoppingMall/run_sample.ps1': '6ecd5cff6e613a56e5b1751eefde5c62669239356445c271993b980133bf981d',
  'framework/languages/dotnet/samples/ShoppingMall/run_sample.sh': '3177b62edf24174142c52481d50f4baa40f15c4c1bd63137582b94c49f352238',
  'framework/languages/dotnet/samples/TicTacToe/run_sample.ps1': 'b3ccc2d17e50143d3b47d7b25f22269a6d39cb5754b1d512d8b5ba0332ce2583',
  'framework/languages/dotnet/samples/TicTacToe/run_sample.sh': '05545a4730a5d0de79798bd552d2f78d92a664d79a160d0b7424a34a9a43bd13',
  'framework/languages/dotnet/samples/ZoneWorld/run_sample.sh': '682d2399c88c49d4d394c6ca93b1275898383a83cfdbbdaf4f2c2ff2a29817bd',
  'framework/languages/java/samples/java/Bingo/run_sample.ps1': '1c99bf748d9ccf6f7b928ca2927ac569578349c1160357d2575ed8e0319ee531',
  'framework/languages/java/samples/java/DeliveryDispatch/run_sample.sh': 'f686c7de588df549640c934f9b18c33599f4abd085fcc6f3ce231a977b95b7b9',
  'framework/languages/java/samples/java/SupportChat/run_sample.sh': '5b02f3598786cb6cf75effb494a2c94f646fcb69da072bdce8fec8eeef2c00d1',
  'framework/languages/java/samples/java/TicTacToe/run_sample.ps1': '06e8b960d9dd2d8075766d68db8665f837bb525a5dc0425cc01e30cd584107d6',
  'framework/languages/java/samples/java/TicTacToe/run_sample.sh': '0a2b50a17fa4b14e33a83f9ac6a00d4655368c523f5991ef84499cf2c6bdf2f3',
  'framework/languages/java/samples/kotlin/SupportChat/run_sample.ps1': 'a534d1f52f8d80b43845f0702c2e093af5f51c8027afabeebbe70854091f039d',
  'framework/languages/java/samples/kotlin/SupportChat/run_sample.sh': '76aaa5eb40ae6f0ed98fb5b914af4ad1a1fcc7cf62175c78cdd4d386172204aa',
  'framework/languages/java/samples/kotlin/TicTacToe/run_sample.ps1': 'a84811ab46c95d5dcbd62aa6e80bc100d354157a813fea68f0fc055f82e276be',
  'framework/languages/java/samples/kotlin/TicTacToe/run_sample.sh': '4f6e3986834be11a61497013d4abcd182229f43c94eb859a54f728fec1bee422',
};

// Baseline regression files that were renamed or replaced by a stronger test.
// Each replacement keeps the old path as provenance and pins the reviewed new file.
export const reviewedRegressionReplacements = {
  'framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ActorTransferTests.cs': {
    path: 'framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ActorRelocationProtocolTests.cs',
    approvedHash: 'ca6f99e1a6b2926ab00b4707653e2bfff06f424187ee0a49246504d41edbe8ab',
  },
  'framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/LocationEventEmitterTests.cs': {
    path: 'framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/LocationRuntimePollingDiffTests.cs',
    approvedHash: '3be2b3b04e70153feb103ccd6873eefcd7ee0676ad265a2b58c87525c163883c',
  },
  'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/service/ZLinkServiceMailboxSchedulerTest.java': {
    path: 'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceMailboxSchedulerTest.java',
    approvedHash: '86b06555982b2ed295d56023c02c984444d1a7e8e5f39c214085932a08ef18ab',
  },
  'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/service/ZLinkServiceOperationRegistryTest.java': {
    path: 'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistryTest.java',
    approvedHash: '10ffcb52a495aac920194f274f135b164ab5573131448f38cbf53016b2980134',
  },
  'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/service/ZLinkServiceWireCodecTest.java': {
    path: 'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceWireCodecTest.java',
    approvedHash: '1336e86cd00d17b5c8e45b3ed98189c65f7f710dfc33bda4bfb3d05473e9bc62',
  },
  'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/locations/ZLinkAllocatedRoutingIdRuntimeTest.java': {
    path: 'framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/mesh/ZLinkMeshNodeRuntimeTest.java',
    approvedHash: 'a6ebe4658a7ce58e06f4c2fc2d370b5d0b2af056aace4b3e45ad81b7040def62',
  },
  'framework/languages/java/zlink-framework-locations-redis/src/test/java/systems/zlink/framework/locations/redis/ZLinkRedisCrossLanguageTest.java': {
    path: 'framework/languages/java/zlink-framework-locations-redis/src/test/java/systems/zlink/framework/locations/redis/ZLinkRedisLocationRowJsonTest.java',
    approvedHash: '498d28b7c002217ff81249f0f2a71dbfc5db041f098138d1898a43f17a68ae36',
  },
  'framework/languages/node/test/contract/routing-id-allocation.test.js': {
    path: 'framework/languages/node/test/contract/sample-bingo-routing-id-allocation-gate.test.js',
    approvedHash: '5db245b6d0e7e4a4e10993981d741cf7363cd471356eedb27be707255f882c2b',
  },
  'framework/languages/node/test/contract/sample-zoneworld-routing.test.js': {
    path: 'framework/languages/node/test/contract/sample-zoneworld-gate.test.js',
    approvedHash: '25fb3bee4563be59ae5dc12bb54566d360dc163c341a9a5b0de2e11aaf612ca4',
  },
};

const rules = [
  {
    id: 'publish-monitoring-removal',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      const name = normalizedMemberName(member);
      const targetCount = /^(?:remote|local)(?:snapshot|admitted|dropped)count$/u.test(name)
        || /^(?:target|drop)count$/u.test(name);
      return /logicalmulticastsnapshot(?:t)?$/u.test(owner)
        || (/meshnodesnapshot(?:t)?$/u.test(owner) && name === 'multicast')
        || (/(?:meshruntimeevent|messageflowevent)(?:t)?$/u.test(owner) && targetCount);
    },
    decisions: ['CA-D77'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'deferred-actor-join',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      const name = normalizedMemberName(member);
      return /actorjoin(?:call|result)/u.test(owner)
        || name === 'typedactorjoinresultt'
        || (isKotlinSourcePackageMember(member)
          && /^(?:awaitjoin|awaitjoinreply)$/u.test(name));
    },
    decisions: ['CA-D74'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'object-context-composition',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      const name = normalizedMemberName(member);
      return (portableOwner(member.ownerIdentity) === 'actor' && name === 'actorid')
        || (/zlinkentryspot$/u.test(owner) && name === 'context');
    },
    decisions: ['CA-D75'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'infrastructure-relocation-callback-removal',
    matches: (_value, member) =>
      /^(?:onactorrelocated(?:async|suspending)?|on_actor_relocated)$/u
        .test(normalizedMemberName(member)),
    decisions: ['CA-D40'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'unified-message-context',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      return /(?:handlercontextt|zlinkhandlercontext|routehandlercontextt|zlinkroutesendcontext|zlinkrouterequestcontext|zlinksendcontext|zlinkrequestcontext|publishcontextt|zlinkpublishcontext|spotactorsendcontextt|spotactorrequestcontextt|spotactorreplyoptionst|zlinkspotactorsendcontext|zlinkspotactorrequestcontext|zlinkspotactorreplyoptions|zlinksessiondispatchcontext|handlerinvocationcontextt|zlinkhandlerinvocation|zlinkinvocationcontext)$/u.test(owner);
    },
    decisions: ['CA-D76'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'one-way-result-removal',
    matches: (_value, member) =>
      /(?:logicalmulticastdetail|publishresult|submitresult|submitstatus)(?:t)?$/u
        .test(normalizedOwner(member)),
    decisions: ['CA-D72'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'one-way-terminator-rename',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      const name = normalizedMemberName(member);
      const dotnetOneWayCall = member.language === 'dotnet'
        && /izlink(?:actor)?sendcall$|izlinkboundsessionsendcall$|izlinkfanoutpublishcall$|izlinkpublishcall$|izlinksessionsendcall$|izlinksessionreplycall$/u.test(owner)
        && name === 'submitasync';
      const kotlinOneWay = member.language === 'kotlin'
        && (/zlinkframeworkextensionskt$/u.test(owner) || isKotlinSourcePackageMember(member))
        && /^(?:send|publishtotopic)$/u.test(name);
      return dotnetOneWayCall || kotlinOneWay;
    },
    decisions: ['CA-D72', 'CA-D73'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'async-terminator-rename',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      const name = normalizedMemberName(member);
      const cppOrNodeCall = /(?:actorrequestcallt|channelrequestcallt|requestcallt|workercallt|zlinkworkercall)$/u
        .test(owner) && /^(?:async|asyncmessage)$/u.test(name);
      const kotlinSourceCall = isKotlinSourcePackageMember(member)
        && /^(?:awaitreply|request|requesttoactorawait|yieldreply|yieldworker)$/u.test(name);
      const kotlinJvmCall = member.language === 'kotlin'
        && /zlinkframeworkextensionskt$/u.test(owner)
        && /^(?:awaitreply|request|requesttoactorawait|yieldreply|yieldworker)$/u.test(name);
      return cppOrNodeCall || kotlinSourceCall || kotlinJvmCall;
    },
    decisions: ['CA-D73'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'redis-transfer-store-split',
    matches: value => /redis.*checkpoint|checkpoint.*redis/u.test(value),
    decisions: ['CA-D31', 'CA-D34', 'CA-D36'],
    coverage: () => ['e2e:add:redis-stores-shared-deployment'],
  },
  {
    id: 'transfer-store-vocabulary',
    matches: value => /checkpoint|transferstore|transferreference|transferstored/u.test(value)
      && !/redis/u.test(value),
    decisions: ['CA-D31', 'CA-D36'],
    coverage: () => ['e2e:add:relocation-store-required-registration'],
  },
  {
    id: 'relocation-vocabulary-breaking-rename',
    matches: value => /transfer/u.test(value)
      && !/checkpoint|transferstore|transferreference|transferstored|redis|forward|routecache/u.test(value),
    decisions: ['CA-D36', 'CA-D37', 'CA-D38', 'CA-D39', 'CA-D40', 'CA-D41', 'CA-D42', 'CA-D43'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'host-lifecycle-topology-state-separation',
    matches: (_value, member) => {
      const owner = normalizedOwner(member);
      const name = normalizedMemberName(member);
      return /(?:drainstate|drainevent|meshnodestate|operationalstate|operationalreason)(?:t)?$/u
        .test(owner)
        || (/drainforcereasont$/u.test(owner)
          && /^(?:deadlineexceeded|teardownfailed)$/u.test(name))
        || (/forcestoppedt$/u.test(owner) && name === 'reason')
        || (/appt$/u.test(owner) && name === 'deprecated')
        || (/(?:frameworkruntimestate|frameworkrelocationoutcome)(?:t)?$/u.test(owner)
          && name === 'drained');
    },
    decisions: ['CA-D79'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'instance-spot-explicit-create',
    matches: value => /instancespotaddress|requesttospot|sendtospot/u.test(value)
      && !/spotrid/u.test(value),
    decisions: ['CA-D25'],
    coverage: () => ['e2e:add:global-spot-explicit-create'],
  },
  {
    id: 'automatic-routing-id',
    matches: value => /routingidslot|allocatedroutingid|routingidallocation|useallocatedroutingid|setroutingidallocation/u.test(value),
    decisions: ['CA-D20', 'CA-D21'],
    coverage: () => ['e2e:add:automatic-rid-collision'],
  },
  {
    id: 'exact-session-bind',
    matches: value => /sessionactors.*bind|bindasync|bindactor|enableactordispatch/u.test(value),
    decisions: ['CA-D05', 'CA-D06'],
    coverage: () => ['e2e:add:exact-generation-mutation-bind'],
  },
  {
    id: 'exact-object-mutation',
    matches: value => /destroy|closespot|closeasync|spotmanagerclose/u.test(value),
    decisions: ['CA-D27'],
    coverage: () => ['e2e:add:exact-generation-mutation-bind'],
  },
  {
    id: 'opaque-object-capability',
    matches: value => /objectcapability(?:.*)(?:readablestatecontractids|type)/u.test(value),
    decisions: ['CA-D14', 'CA-D37', 'CA-D39'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'spot-context-logical-identity',
    matches: value => /spotcommoncontext(?:.*)(?:spotname|routingid)/u.test(value),
    decisions: ['CA-D01', 'CA-D02'],
    coverage: () => ['e2e:add:global-spot-explicit-create'],
  },
  {
    id: 'spot-id-string-identity',
    matches: value => /spotrid/u.test(value),
    decisions: ['CA-D65'],
    coverage: member => [
      'e2e:add:global-spot-explicit-create',
      `public-behavior:formal-contract-parity:${member.language}`,
    ],
  },
  {
    id: 'yield-surface-restriction',
    matches: (value, member) => /yield/u.test(value)
      && !/actorjoin/u.test(normalizedOwner(member))
      && !(isKotlinSourcePackageMember(member)
        && /^(?:yieldreply|yieldworker)$/u.test(normalizedMemberName(member)))
      && !(member.language === 'kotlin'
        && /zlinkframeworkextensionskt$/u.test(normalizedOwner(member))
        && /^(?:yieldreply|yieldworker)$/u.test(normalizedMemberName(member))),
    decisions: ['CA-D58', 'CA-D59'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'spot-actor-lifecycle-split',
    matches: value => /(?:entryspot|spotactorlifecycle).*(?:onactorjoin|onjoinedactor|onleaveactor|ondisconnectactor)/u.test(value),
    decisions: ['CA-D67'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'authority-create-transition-split',
    matches: value => /authorityexpect(?:missing|found)|authorityexpectationzlinkauthorityexpectation|authoritygenerationtransition.*newobject|missingmissing/u.test(value)
      && !/equals|hashcode|tostring/u.test(value),
    decisions: ['CA-D44', 'CA-D45'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'reviewed-exact-surface-cleanup',
    matches: (value, member) =>
      /zlinkentryspotcontext|zlinkframeworkruntimeeventruntime|zlinkhandlerinvocation(?:context|message)/u
        .test(value)
      && !/spotrid|zlinkhandlerinvocation(?:context|message)/u.test(value)
      && !(/zlinkentryspot$/u.test(normalizedOwner(member))
        && normalizedMemberName(member) === 'context'),
    decisions: ['CA-D29'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'actor-spot-relocation',
    matches: (value, member) => /joinentryspot/u.test(value)
      && !/actorjoin/u.test(normalizedOwner(member)),
    decisions: ['CA-D07'],
    coverage: () => ['e2e:add:same-node-join-without-relocation-payload'],
  },
  {
    id: 'session-actor-bind-reference',
    matches: value => /sessionactormanager|sessionactor(?:t|\b)/u.test(value),
    decisions: ['CA-D05'],
    coverage: () => ['e2e:add:exact-generation-mutation-bind'],
  },
  {
    id: 'actor-ref-record-helper',
    matches: value => /actorrefsnapshot.*(?:equals|hashcode|tostring)/u.test(value),
    decisions: ['CA-D29'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'actor-ref-contract',
    matches: value => /actorrefsnapshot|actorref/u.test(value)
      && !/iszlinkframeworkerrorretriablebydefault|equals|hashcode|tostring/u.test(value),
    decisions: ['CA-D01'],
    coverage: () => ['e2e:add:global-actor-remote-create'],
  },
  {
    id: 'actor-directory-query',
    matches: value => /actordirectory|findactor/u.test(value),
    decisions: ['CA-D26'],
    coverage: () => ['e2e:add:global-actor-remote-create'],
  },
  {
    id: 'actor-spot-handle-query',
    matches: value => /actorspothandleresolver|resolveactorspothandle/u.test(value),
    decisions: ['CA-D01', 'CA-D26'],
    coverage: () => ['e2e:add:global-actor-remote-create'],
  },
  {
    id: 'spot-manager-replacement',
    matches: value => /spothandleresolver|resolvespothandle|spothandle|spotmanager.*(?:find|list)|spotinfo/u.test(value)
      && !/actorspothandleresolver|resolveactorspothandle|spotrid/u.test(value),
    decisions: ['CA-D02', 'CA-D26'],
    coverage: () => ['e2e:add:global-spot-explicit-create'],
  },
  {
    id: 'actor-global-authority-key',
    matches: value => /actorlocationkey|authoritysnapshot.*actor/u.test(value),
    decisions: ['CA-D01'],
    coverage: member => [`regression:add:global-authority-key:${runtimeLanguage(member.language)}`],
  },
  {
    id: 'spot-global-authority-key',
    matches: value => /spotlocationkey|authoritysnapshot.*spot/u.test(value)
      && !/spotrid/u.test(value),
    decisions: ['CA-D02'],
    coverage: member => [`regression:add:global-authority-key:${runtimeLanguage(member.language)}`],
  },
  {
    id: 'instance-spot-activation-timeout',
    matches: value => /instancespotfactoryoptions.*(?:activationtimeout|fromseconds)/u.test(value),
    decisions: ['CA-D04', 'CA-D10'],
    coverage: () => ['e2e:add:global-spot-explicit-create'],
  },
  {
    id: 'instance-spot-options-record-helper',
    matches: value => /instancespotfactoryoptions.*(?:equals|hashcode|tostring)/u.test(value),
    decisions: ['CA-D29'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'instance-spot-options-constructor',
    matches: value => /instancespotfactoryoptions.*instancespotfactoryoptions/u.test(value),
    decisions: ['CA-D04', 'CA-D10', 'CA-D23'],
    coverage: () => [
      'e2e:add:global-spot-explicit-create',
      'e2e:add:placement-capacity-weight',
    ],
  },
  {
    id: 'instance-spot-active-capacity',
    matches: value => /instancespotfactoryoptions.*maxactiveinstances/u.test(value),
    decisions: ['CA-D23'],
    coverage: () => ['e2e:add:placement-capacity-weight'],
  },
  {
    id: 'mesh-node-spot-types',
    matches: value => /meshNodeDescriptor.*spotTypes/i.test(value),
    decisions: ['CA-D14'],
    coverage: () => ['e2e:add:placement-capacity-weight'],
  },
  {
    id: 'object-capability-availability',
    matches: value => /objectcapability.*available/u.test(value),
    decisions: ['CA-D23'],
    coverage: () => ['e2e:add:placement-capacity-weight'],
  },
  {
    id: 'placement-policy',
    matches: value => /placement|capacity|weight/u.test(value)
      && !/zlinksocket/u.test(value),
    decisions: ['CA-D14', 'CA-D22', 'CA-D23', 'CA-D70'],
    coverage: () => ['e2e:add:placement-capacity-weight'],
  },
  {
    id: 'reservation-recovery',
    matches: value => /reservation|reserve|commitreservation|abortreservation/u.test(value),
    decisions: ['CA-D24'],
    coverage: () => ['e2e:add:reservation-crash-recovery'],
  },
  {
    id: 'forwarding-and-cache',
    matches: value => /forwardwindow|forwarding|routecache|route.*cache/u.test(value),
    decisions: ['CA-D16', 'CA-D17'],
    coverage: () => ['e2e:add:forwarding-bounds'],
  },
  {
    id: 'actor-factory-registration',
    matches: value => /meshnodebuilder.*(?:add)?actorfactory/u.test(value),
    decisions: ['CA-D14', 'CA-D18', 'CA-D28'],
    coverage: () => ['sample:add:remote-object-create'],
  },
  {
    id: 'actor-client-messaging',
    matches: value => /actorclient.*(?:requesttoactor|sendtoactor)/u.test(value),
    decisions: ['CA-D01', 'CA-D26'],
    coverage: () => ['e2e:add:global-actor-remote-create'],
  },
  {
    id: 'actor-fluent-create',
    matches: value => /actormanager.*create|actormanager.*getorcreate/u.test(value),
    decisions: ['CA-D01', 'CA-D04', 'CA-D09'],
    coverage: () => ['e2e:add:global-actor-remote-create'],
  },
  {
    id: 'spot-fluent-create',
    matches: (value, member) => /spotmanager.*create|spotmanager.*getorcreate|spotcreate/u.test(value)
      && !/meshnodebuilder|nestmeshnodebuilder/u.test(normalize(member.ownerIdentity))
      && !/maxactiveinstances|spotrid/u.test(value),
    decisions: ['CA-D02', 'CA-D04', 'CA-D09'],
    coverage: () => ['e2e:add:global-spot-explicit-create'],
  },
  {
    id: 'closed-object-role',
    matches: value => /addentryspot|configureentryspot|entryspotoptions|meshnodebuilder.*add.*spot/u.test(value)
      && !/entryspotoptions.*routingid|setentryspotroutingid/u.test(value),
    decisions: ['CA-D18', 'CA-D28'],
    coverage: () => ['sample:add:remote-object-create'],
  },
  {
    id: 'operational-location-query',
    matches: value => /peerlocationresolver|listlivepeers|locationruntimequery|list.*location|listtopology|listmeshnode|listauthorities|readauthority|listclientservers|listfanoutpublishers|listpeers|listroutes/u.test(value)
      && !/resolve.*handle/u.test(value),
    decisions: ['CA-D26'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'fixed-entry-routing-id',
    matches: value => /entryspotoptions.*routingid|setentryspotroutingid/u.test(value),
    decisions: ['CA-D19'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'kotlin-reviewed-contract-set',
    matches: (value, member) => member.language === 'kotlin'
      && /extensionskt|zlinksuspending|package/u.test(value),
    decisions: ['CA-D29'],
    coverage: () => ['public-behavior:formal-contract-parity:kotlin'],
  },
  {
    id: 'actor-location-filter',
    matches: value => /actorlocationfilter/u.test(value)
      && !/spotrid/u.test(value),
    decisions: ['CA-D01', 'CA-D26'],
    coverage: () => ['e2e:add:global-actor-remote-create'],
  },
  {
    id: 'spot-location-filter',
    matches: value => /spotlocationfilter/u.test(value)
      && !/spotrid/u.test(value),
    decisions: ['CA-D02', 'CA-D26'],
    coverage: () => ['e2e:add:global-spot-explicit-create'],
  },
  {
    id: 'unified-message-context',
    matches: value => /spotactormessagemetadata|spotpacketcontext|streamdispatchcontext|streammetadata/u.test(value),
    decisions: ['CA-D76'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'node-reviewed-contract-set',
    matches: (value, member) => member.language === 'node'
      && /zlinksocket|zlinkspotevent|zlinkdecoratormetadata|zlinkframeworkerrorkindvalues|zlinkspotactorrequest|zlinkspotactorreplyoptions|zlinkspotpeer|zlinksession|zlinkactorjoinresult|iszlinkframeworkerrorretriablebydefault/i.test(value)
      && !/zlinkactorjoinresult|zlinksessiondispatchcontext|zlinkspotactorreplyoptions|zlinkspotactorrequestcontext/i.test(value),
    decisions: ['CA-D29'],
    coverage: () => ['public-behavior:formal-contract-parity:node'],
  },
  {
    id: 'generated-record-contract-set',
    matches: value => /zlinkmeshNodeDescriptorhashset|zlinkauthorityexpectmissing(?:equals|hashcode|tostring)/i.test(value),
    decisions: ['CA-D29'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'cpp-reviewed-contract-set',
    matches: (value, member) => member.language === 'cpp'
      && /(?:actorlocation|spotlocation|routelocation).*from|appt(?:retire|shutdown)|(?:clientserverchannelserverbuilder|meshchannelserverbuilder|meshnodebuilder).*(?:addrequesthandler|addsendhandler|addrouterequesthandler|addroutesendhandler)|healthbuildertsetstatus|loggert(?:critical|debug|error|info|log|trace|warn)|loggingbuildert(?:useasync|userotatingfile)|metricsbuildertrecordruntimemetric|requestclientt(?:request|send)|spotcommoncontexttaddtimer/u.test(value)
      && !/spotlocationkey/u.test(value),
    decisions: ['CA-D29'],
    coverage: () => ['public-behavior:formal-contract-parity:cpp'],
  },
  {
    id: 'node-generated-contract-set',
    matches: (value, member) => member.language === 'node'
      && /zlinkauthoritykeybrand|zlinkauthorityversionbrand|zlinkauthoritykeytrue|zlinkauthoritystoreversiontrue|zlinkmoduleoptionstrue|zlinkmetercreate(?:counter|histogram|updowncounter)/u.test(value),
    decisions: ['CA-D29'],
    coverage: () => ['public-behavior:formal-contract-parity:node'],
  },
  {
    id: 'public-boundary-internal-surface-cleanup',
    matches: () => true,
    decisions: ['CA-D29'],
    coverage: member => [`public-behavior:formal-contract-parity:${member.language}`],
  },
  {
    id: 'redis-location-options-split',
    matches: value => /mutable.*redislocationoptions/u.test(value),
    decisions: ['CA-D34'],
    coverage: () => ['e2e:add:redis-stores-shared-deployment'],
  },
];

export function auditRemovedMemberBehavior(member) {
  const value = normalize(`${member.ownerIdentity}.${member.memberName}`);
  const kotlinReviewedRule = rules.find(rule => rule.id === 'kotlin-reviewed-contract-set');
  const publicBoundaryCleanupRule = rules.find(
    rule => rule.id === 'public-boundary-internal-surface-cleanup');
  const domainMatches = rules.filter(rule => rule !== kotlinReviewedRule
      && rule !== publicBoundaryCleanupRule)
    .filter(candidate => candidate.matches(value, member));
  const matches = domainMatches.length > 0
    ? domainMatches
    : kotlinReviewedRule.matches(value, member)
      ? [kotlinReviewedRule]
      : [publicBoundaryCleanupRule];
  if (matches.length !== 1) {
    return {
      state: matches.length === 0 ? 'unmatched' : 'ambiguous',
      ruleIds: matches.map(rule => rule.id),
    };
  }
  const [rule] = matches;
  return {state: 'matched', behavior: {
    ruleId: rule.id,
    decisionCoverage: rule.decisions,
    replacementCoverage: rule.coverage(member),
  }};
}

export function removedMemberBehavior(member) {
  const audit = auditRemovedMemberBehavior(member);
  if (audit.state !== 'matched') {
    throw new Error(`${audit.state}:${audit.ruleIds.join(',')} removed public member: ${member.identity}`);
  }
  return audit.behavior;
}
