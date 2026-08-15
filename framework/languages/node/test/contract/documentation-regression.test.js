const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const docRoot = path.join(workspaceRoot, '..', '..', 'doc', 'framework', 'node');
const specRoot = path.join(
  workspaceRoot,
  '..',
  '..',
  'doc',
  'framework',
  'common',
  'spec',
  'server',
  'languages',
  'node'
);
const samplesRoot = path.join(workspaceRoot, 'samples');
const commonSpecRoot = path.resolve(specRoot, '..', '..');
const typescriptSpecRoot = path.resolve(
  specRoot,
  '..',
  '..',
  '..',
  'stream-connector',
  'languages',
  'typescript'
);
const guideFiles = [
  '01-overview.ko.md',
  '02-getting-started.ko.md',
  '03-concepts.ko.md',
  '04-channel-messaging.ko.md',
  '05-spot.ko.md',
  '06-actor-session.ko.md',
  '07-stream.ko.md',
  '08-registry.ko.md',
  '09-monitoring.ko.md',
  '10-feature-map.ko.md',
  '11-interface-catalog.ko.md',
  '12-grpc-alternative.ko.md'
];

test('node documentation does not restore the removed legacy guide chapters', () => {
  const restored = guideFiles.filter((file) => fs.existsSync(path.join(docRoot, 'guide', file)));
  assert.deepEqual(restored, []);
});

test('node README does not link removed legacy guide chapters', () => {
  const readme = fs.readFileSync(path.join(docRoot, 'README.ko.md'), 'utf8');
  const stale = guideFiles.filter((file) => readme.includes(`guide/${file}`));
  assert.deepEqual(stale, []);
});

test('node interface specification documents the current execution-turn APIs', () => {
  const specification = readNodeInterfaceCatalog();

  assert.doesNotMatch(specification, /runWorker\(/);
  for (const api of ['runCpuWorker', 'runIoWorker', 'submit()', 'yield()']) {
    assert.match(specification, new RegExp(api.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')));
  }
  assert.doesNotMatch(specification, /public turn 반납 API가 필요하지 않다/);
});

test('Node RouteMesh contract is owned by the canonical spec and implementation', () => {
  const canonicalSpec = fs.readFileSync(
    path.join(specRoot, '01-system-structure.ko.md'),
    'utf8'
  );
  const builderSource = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Configuration', 'Builders.ts'),
    'utf8'
  );

  assert.match(canonicalSpec, /`addRouteMesh\(meshName\)`/);
  assert.match(builderSource, /addRouteMesh\(meshName: string\)/);
  assert.equal(
    fs.existsSync(path.join(workspaceRoot, '..', '..', 'doc', 'plan', 'node-framework-spec-gap-ledger')),
    false,
    'temporary gap ledgers must not remain as a public documentation dependency'
  );
});

test('node documentation relative markdown links resolve', () => {
  const markdownFiles = allMarkdownFiles(docRoot);
  const broken = [];

  for (const file of markdownFiles) {
    const content = fs.readFileSync(file, 'utf8');
    for (const link of markdownLinks(content)) {
      if (isExternalOrRoot(link)) {
        continue;
      }
      const target = path.resolve(path.dirname(file), link.split('#')[0]);
      if (!fs.existsSync(target)) {
        broken.push(`${path.relative(workspaceRoot, file)} -> ${link}`);
      }
    }
  }

  assert.deepEqual(broken.sort(), []);
});

test('node spec and internals documentation do not depend on legacy draft links', () => {
  const checkedRoots = [
    specRoot,
    path.join(docRoot, 'internals')
  ];
  const offenders = [];

  for (const root of checkedRoots) {
    for (const file of allMarkdownFiles(root)) {
      const content = fs.readFileSync(file, 'utf8');
      for (const link of markdownLinks(content)) {
        if (link.includes('../draft/') || link.includes('/draft/')) {
          offenders.push(`${path.relative(workspaceRoot, file)} -> ${link}`);
        }
      }
    }
  }

  assert.deepEqual(offenders.sort(), []);
});

test('node documentation does not restore the removed embedded Registry contract', () => {
  assert.equal(
    fs.existsSync(path.join(specRoot, '01-system-structure.ko.md')),
    true,
    'Node package, host registration and lifecycle contract is owned by system-structure'
  );

  const forbidden = [
    /ZLinkRegistryBackendAdapter/,
    /ZLinkRegistryModule/,
    /ZLINK_REGISTRY_QUERY/,
    /RegistryAndMonitoring/,
    /embedded Registry/i
  ];
  const offenders = [];
  for (const root of [specRoot, docRoot]) {
    for (const file of allMarkdownFiles(root)) {
      const content = fs.readFileSync(file, 'utf8');
      for (const pattern of forbidden) {
        if (pattern.test(content)) {
          offenders.push(`${path.relative(workspaceRoot, file)}: ${pattern.source}`);
        }
      }
    }
  }

  assert.deepEqual(offenders.sort(), []);
});

test('node README links the canonical framework specifications', () => {
  const required = [
    ['01-system-structure.ko.md', '../common/spec/server/languages/node/01-system-structure.ko.md'],
    ['interfaces/README.ko.md', '../common/spec/server/languages/node/interfaces/README.ko.md']
  ];
  const missing = [];
  const languageReadme = fs.readFileSync(path.join(specRoot, 'README.ko.md'), 'utf8');
  const nodeReadme = fs.readFileSync(path.join(docRoot, 'README.ko.md'), 'utf8');

  for (const [file, nodeReadmeLink] of required) {
    if (!fs.existsSync(path.join(specRoot, file))) missing.push(`${file}: missing`);
    if (!languageReadme.includes(`](${file})`)) missing.push(`${file}: language README link`);
    if (!nodeReadme.includes(nodeReadmeLink)) {
      missing.push(`${file}: Node README link`);
    }
  }

  assert.deepEqual(missing.sort(), []);
});

test('node specifications keep key Spot signatures aligned with public declarations', () => {
  const systemSpec = fs.readFileSync(path.join(specRoot, '01-system-structure.ko.md'), 'utf8');
  const interfaceSpec = readNodeInterfaceCatalog();
  const spotContracts = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Spots', 'Contracts.ts'),
    'utf8'
  );
  const frameworkBuilders = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Configuration', 'Builders.ts'),
    'utf8'
  );

  const requiredSignatures = [
    /create\(spotType: string\): ZLinkSpotCreateCall/,
    /getOrCreate\(\s*spotId: SpotId,\s*spotType: string\): ZLinkSpotGetOrCreateCall/,
    /find\(spotId: SpotId, signal\?: AbortSignal\): Promise<SpotRef \| undefined>/,
    /close\(spot: SpotRef, signal\?: AbortSignal\): Promise<boolean>/
  ];
  for (const signature of requiredSignatures) {
    assert.match(interfaceSpec, signature);
    assert.match(spotContracts, signature);
  }

  const peerConnections = /peerConnections\(\): ZLinkMeshPeerConnections/;
  assert.match(interfaceSpec, peerConnections);
  assert.match(frameworkBuilders, peerConnections);

  for (const token of [
    'ZLINK_BOUND_SESSION_FACTORY',
    'ZLINK_CHANNEL_RUNTIME_OPTIONS',
  ]) {
    const alwaysAvailable = systemSpec.split('**역할이 있을 때만 등록되는 provider:**')[0];
    assert.equal(alwaysAvailable.includes(`| \`${token}\``), true);
  }

  const conditionalProviders = systemSpec.split('**역할이 있을 때만 등록되는 provider:**')[1];
  for (const row of [
    /\| `ZLINK_ACTOR_CLIENT` \| MeshNode와 location store가 모두 등록됨 \|/,
    /\| `ZLINK_ACTOR_MANAGER` \| actor manager가 활성화됨 \|/,
    /\| `ZLINK_LOCATION_RUNTIME_QUERY` \| location store가 하나 이상 등록됨 \|/
  ]) {
    assert.match(conditionalProviders, row);
  }
});

test('node one-way submit documentation keeps the bounded admission contract', () => {
  const calls = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Channels', 'Calls.ts'),
    'utf8'
  );
  const matrix = fs.readFileSync(
    path.join(docRoot, 'internals', 'regression-test-matrix.ko.md'),
    'utf8'
  );

  assert.match(calls, /export interface ZLinkSendCall \{[\s\S]*submit\(signal\?: AbortSignal\): Promise<void>;/);
  assert.match(calls, /export interface ZLinkPublishCall \{[\s\S]*submit\(signal\?: AbortSignal\): Promise<void>;/);
  assert.doesNotMatch(calls, new RegExp(['try', 'Submit'].join('')));
  assert.match(matrix, /`submit\(\)`은 bounded\s+admission 결과를 비동기로 반환/);
  assert.doesNotMatch(matrix, /Promise.*미해결 유지|ready 이후에 resolve|submitter timeout 정책에 따라 resolve/);
});

test('node canonical interface catalog has unique declaration ownership', () => {
  const catalog = readNodeInterfaceCatalog();
  const catalogShapes = new Map();

  for (const declaration of catalogTypeDeclarations(catalog)) {
    const values = catalogShapes.get(declaration.name.text) ?? [];
    values.push(declarationShape(declaration));
    catalogShapes.set(declaration.name.text, values);
  }

  const duplicates = [...catalogShapes]
    .filter(([, declarations]) => declarations.length !== new Set(declarations).size)
    .map(([name]) => name)
    .sort();

  assert.equal(catalogShapes.size > 0, true);
  assert.deepEqual(duplicates, []);
});

test('typescript stream connector specification matches the browser package declaration', () => {
  const specification = fs.readFileSync(path.join(typescriptSpecRoot, '03-stream-connector.ko.md'), 'utf8');
  const connector = catalogTypeDeclarations(specification)
    .find((declaration) => declaration.name.text === 'ZlinkStreamConnector');
  const publicShapes = publicDeclarationShapes([
    path.join(workspaceRoot, 'packages', 'stream-connector', 'dist', 'browser', 'index.d.ts')
  ]);

  assert.notEqual(connector, undefined);
  assert.deepEqual(
    [declarationShape(connector)],
    [...new Set(publicShapes.get('ZlinkStreamConnector') ?? [])]
  );
  assert.match(specification, /waitFor<TPayload = ZlinkStreamEncodedPayload>\(name: string\)/);
  assert.match(specification, /on<TPayload = ZlinkStreamEncodedPayload>\(/);
  assert.match(specification, /flowFrom\(flow: ZlinkStreamFlow\): ZlinkStreamSendCall/);
  assert.match(specification, /flowFrom\(flow: ZlinkStreamFlow\): ZlinkStreamRequestCall/);
});

test('canonical common spec owns server semantics without the deleted duplicate tree', () => {
  const deletedSpecRoot = path.join(workspaceRoot, '..', '..', 'doc', 'framework', 'spec');
  const required = [
    '07-channel-topology.ko.md',
    '15-spot-actor.ko.md',
    '19-stream-session.ko.md',
    '29-transport-liveness.ko.md'
  ];

  assert.equal(fs.existsSync(deletedSpecRoot), false);
  assert.deepEqual(required.filter((file) => !fs.existsSync(path.join(commonSpecRoot, file))), []);
});

test('node documentation keeps fanout and route client public surface aligned with contracts', () => {
  const files = allMarkdownFiles(path.join(specRoot, 'interfaces'));
  const offenders = [];

  for (const file of files) {
    const content = fs.readFileSync(file, 'utf8');
    const relative = path.relative(workspaceRoot, file);
    if (/publishToChannel/.test(content) && !/호환 alias로 남기지 않는다/.test(content)) {
      offenders.push(`${relative}: removed fanout publish name`);
    }
    if (/public client 로 노출하지 않는다|internal-only/.test(content)) {
      offenders.push(`${relative}: route client described as internal-only`);
    }
  }

  assert.deepEqual(offenders.sort(), []);
  const channelSpec = readNodeInterfaceCatalog();
  assert.match(channelSpec, /publish\(channelName: string, topic: string, event: unknown\)/);
});

test('node framework docs do not describe removed nested configuration callbacks', () => {
  const checkedRoots = [
    path.join(docRoot, 'guide'),
    specRoot,
    path.join(docRoot, 'internals')
  ];
  const offenders = [];
  const forbidden = [
    [/Add(ClientServerChannel|FanoutChannel|DealerMeshChannel|RouteMeshChannel|SpotNode|StreamNode)\([^`\n)]*,\s*[a-zA-Z_][^`\n)]*=>/, 'dotnet nested channel callback'],
    [/Enable(Server|Client|Publisher|Subscriber)\([^`\n)]*=>/, 'dotnet nested capability callback'],
    [/clientServerChannel\s*\([^`\n)]*,\s*\(/, 'old NestJS clientServerChannel callback'],
    [/fanoutChannel\s*\([^`\n)]*,\s*\(/, 'old NestJS fanoutChannel callback'],
    [/routerMesh\s*\([^`\n)]*,\s*\(/, 'old NestJS routerMesh callback'],
    [/spotNode\s*\([^`\n)]*,\s*\(/, 'old NestJS spotNode callback'],
    [/streamNode\s*\([^`\n)]*,\s*\(/, 'old NestJS streamNode callback'],
    [/ZLinkModule\.forRoot\s*\(\s*\{/, 'old NestJS object module options'],
    [/\bclientServerChannels\s*:\s*\{/, 'old NestJS clientServerChannels object option'],
    [/\bclientServerChannels(?:\[[^\]]+\]|\.[A-Za-z0-9_-]+)/, 'old NestJS clientServerChannels object reference'],
    [/\bfanoutChannels\s*:\s*\{/, 'old NestJS fanoutChannels object option'],
    [/\bfanoutChannels(?:\[[^\]]+\]|\.[A-Za-z0-9_-]+)/, 'old NestJS fanoutChannels object reference'],
    [/\bdealerMeshChannels\s*:\s*\{/, 'old NestJS dealerMeshChannels object option'],
    [/\bdealerMeshChannels(?:\[[^\]]+\]|\.[A-Za-z0-9_-]+)/, 'old NestJS dealerMeshChannels object reference'],
    [/\brouterMeshes\s*:\s*\{/, 'old NestJS routerMeshes object option'],
    [/\brouterMeshes(?:\[[^\]]+\]|\.[A-Za-z0-9_-]+)/, 'old NestJS routerMeshes object reference'],
    [/\bspotNodes(?:\s*:\s*\{|\s*:\s*\[|\[[^\]]+\]|\.[A-Za-z0-9_-]+)/, 'old NestJS spotNodes object option'],
    [/\bspotMeshes(?:\s*:\s*\{|\[[^\]]+\])/, 'old NestJS spotMeshes object option'],
    [/\bacceptedSpotRouteChannels(?:\s*:\s*\{|\[[^\]]+\])/, 'old NestJS acceptedSpotRouteChannels option'],
    [/\bmanualConnections\s*:/, 'old NestJS manualConnections object option'],
    [/codecs\s*:\s*\[/, 'old codecs array option']
  ];

  for (const root of checkedRoots) {
    for (const file of allMarkdownFiles(root)) {
      const content = fs.readFileSync(file, 'utf8');
      const relative = path.relative(workspaceRoot, file);
      for (const [pattern, reason] of forbidden) {
        if (pattern.test(content)) {
          offenders.push(`${relative}: ${reason}`);
        }
      }
    }
  }

  assert.deepEqual(offenders.sort(), []);
});

test('node actor destroy docs keep Entry Spot ownership and disconnect isolation', () => {
  const docs = actorDestroyDocumentationFiles();
  const offenders = [];
  const forbidden = [
    [/destroyActorAsync\b/, 'lower camel async destroy name'],
    [/destroy_actor\b/, 'snake case destroy name'],
    [/\bOnActorLeft\b/, 'legacy PascalCase left callback'],
    [/\bonActorLeft\b/, 'legacy lower camel left callback'],
    [/\bon_actor_left\b/, 'legacy snake case left callback'],
    [/\bOnCreateActor\b/, 'legacy PascalCase create callback'],
    [/\bon_actor_created\b/, 'legacy snake case create callback'],
    [/\bonPostActorJoined\b/, 'legacy post actor joined callback'],
    [/disconnect\s*->\s*destroy/i, 'disconnect-to-destroy arrow wording'],
    [/disconnect[^.\n]*자동[^.\n]*destroy/, 'automatic disconnect destroy wording'],
    [/disconnect[^.\n]*destroy[^.\n]*자동/, 'automatic disconnect destroy wording'],
    [/자동[^.\n]*삭제/, 'automatic deletion wording']
  ];

  for (const file of docs) {
    const content = fs.readFileSync(file, 'utf8');
    const relative = path.relative(workspaceRoot, file);
    for (const [pattern, reason] of forbidden) {
      if (pattern.test(content)) {
        offenders.push(`${relative}: ${reason}`);
      }
    }
  }

  const actorSpec = readNodeInterfaceCatalog();
  const declarations = catalogTypeDeclarations(actorSpec);
  const entryContext = declarations.find((declaration) => declaration.name.text === 'ZLinkEntrySpotContext');
  const userContext = declarations.find((declaration) => declaration.name.text === 'ZLinkSpotContext');
  assert.notEqual(entryContext, undefined);
  assert.notEqual(userContext, undefined);
  assert.match(declarationShape(entryContext), /destroyActor\(actor:TActor,signal\?:AbortSignal\):Promise<void>/);
  assert.doesNotMatch(declarationShape(userContext), /destroyActor/);
  assert.deepEqual(offenders.sort(), []);
});

test('node interface catalog names resolve in public package declarations', () => {
  const frameworkDeclarations = packageDeclarations('framework');
  const nestjsDeclarations = packageDeclarations('nestjs');
  const connectorDeclarations = packageDeclarations('stream-connector');
  const missing = [];

  for (const name of [
    'ZLinkChannelClient',
    'ZLinkFanoutClient',
    'ZLinkSendCall',
    'ZLinkRequestCall',
    'ZLinkPublishCall',
    'ZLinkSpot',
    'ZLinkSpotContext',
    'ZLinkSpotManager',
    'ZLinkActor',
    'ZLinkActorContext',
    'ZLinkBoundSession',
    'ZLinkSession',
    'ZLinkSessionContext',
    'ZLinkSessionClient'
  ]) {
    if (!declarationHasSymbol(frameworkDeclarations, name)) {
      missing.push(`@zlink-systems/framework:${name}`);
    }
  }

  for (const name of [
    'ZLinkModule',
    'ZLINK_CHANNEL_CLIENT',
    'ZLINK_FANOUT_CLIENT',
    'ZLINK_SPOT_MANAGER',
    'ZLINK_ACTOR_MANAGER'
  ]) {
    if (!declarationHasSymbol(nestjsDeclarations, name)) {
      missing.push(`@zlink-systems/nestjs:${name}`);
    }
  }

  for (const name of [
    'ZlinkStreamConnector',
    'ZlinkStreamHeaderCodec',
    'ZlinkStreamFrameCodec'
  ]) {
    if (!declarationHasSymbol(connectorDeclarations, name)) {
      missing.push(`@zlink-systems/stream-connector:${name}`);
    }
  }

  assert.deepEqual(missing, []);
});

function actorDestroyDocumentationFiles() {
  const officialDocs = [
    ...allMarkdownFiles(path.join(docRoot, 'guide')),
    ...allMarkdownFiles(specRoot),
    ...allMarkdownFiles(path.join(docRoot, 'internals')),
    path.join(docRoot, 'README.ko.md')
  ];
  const sampleReadmes = allMarkdownFiles(samplesRoot)
    .filter((file) => path.basename(file) === 'README.ko.md');
  return [...officialDocs, ...sampleReadmes];
}

function allMarkdownFiles(root) {
  const files = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) {
      files.push(...allMarkdownFiles(fullPath));
    } else if (entry.isFile() && entry.name.endsWith('.md')) {
      files.push(fullPath);
    }
  }
  return files;
}

function readNodeInterfaceCatalog() {
  return allMarkdownFiles(path.join(specRoot, 'interfaces'))
    .filter((file) => file.endsWith('.ko.md'))
    .sort()
    .map((file) => fs.readFileSync(file, 'utf8'))
    .join('\n');
}

function packageDeclarations(packageName) {
  const distRoot = path.join(workspaceRoot, 'packages', packageName, 'dist');
  return allDeclarationFiles(distRoot)
    .map((file) => fs.readFileSync(file, 'utf8'))
    .join('\n');
}

function allDeclarationFiles(root) {
  const files = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) {
      files.push(...allDeclarationFiles(fullPath));
    } else if (entry.isFile() && entry.name.endsWith('.d.ts')) {
      files.push(fullPath);
    }
  }
  return files;
}

function declarationHasSymbol(declarations, name) {
  return new RegExp(`\\b(?:export\\s+)?(?:declare\\s+)?(?:interface|class|const|type|function)\\s+${name}\\b`).test(declarations);
}

function publicDeclarationShapes(entries) {
  const program = ts.createProgram(entries, {
    target: ts.ScriptTarget.Latest,
    moduleResolution: ts.ModuleResolutionKind.Node10,
    skipLibCheck: true
  });
  const checker = program.getTypeChecker();
  const shapes = new Map();

  for (const entry of entries) {
    const sourceFile = program.getSourceFile(entry);
    const moduleSymbol = sourceFile === undefined ? undefined : checker.getSymbolAtLocation(sourceFile);
    assert.notEqual(moduleSymbol, undefined, `${entry} is not a declaration module`);
    for (let symbol of checker.getExportsOfModule(moduleSymbol)) {
      if ((symbol.flags & ts.SymbolFlags.Alias) !== 0) symbol = checker.getAliasedSymbol(symbol);
      for (const declaration of symbol.getDeclarations() ?? []) {
        if (!isCatalogTypeDeclaration(declaration)) continue;
        const values = shapes.get(symbol.name) ?? [];
        values.push(declarationShape(declaration));
        shapes.set(symbol.name, values);
      }
    }
  }
  return shapes;
}

function catalogTypeDeclarations(markdown) {
  const declarations = [];
  for (const match of markdown.matchAll(/```(?:ts|typescript)\s*\n([\s\S]*?)```/g)) {
    const sourceFile = ts.createSourceFile(
      'node-interface-catalog.ts',
      match[1],
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TS
    );
    for (const statement of sourceFile.statements) {
      if (ts.isVariableStatement(statement)) {
        declarations.push(...statement.declarationList.declarations.filter(isCatalogTypeDeclaration));
      } else if (isCatalogTypeDeclaration(statement)) {
        declarations.push(statement);
      }
    }
  }
  return declarations;
}

function isCatalogTypeDeclaration(node) {
  return (ts.isInterfaceDeclaration(node)
    || ts.isTypeAliasDeclaration(node)
    || ts.isEnumDeclaration(node)
    || ts.isClassDeclaration(node)
    || ts.isFunctionDeclaration(node)
    || ts.isVariableDeclaration(node))
    && node.name !== undefined;
}

function declarationShape(declaration) {
  const typeParameters = declaration.typeParameters
    ?.map((parameter) => canonicalDeclarationText(parameter.getText()))
    .join(',') ?? '';
  const heritage = declaration.heritageClauses
    ?.map((clause) => canonicalDeclarationText(clause.getText()))
    .join('') ?? '';

  if (ts.isInterfaceDeclaration(declaration) || ts.isClassDeclaration(declaration)) {
    const members = declaration.members
      .map((member) => canonicalDeclarationText(member.getText()))
      .sort()
      .join('|');
    return `${ts.SyntaxKind[declaration.kind]}<${typeParameters}>${heritage}{${members}}`;
  }
  if (ts.isEnumDeclaration(declaration)) {
    const members = declaration.members.map((member) => {
      const name = canonicalDeclarationText(member.name.getText());
      if (member.initializer === undefined) return name;
      const initializer = member.initializer.getText();
      const numeric = Number(initializer);
      return `${name}=${Number.isFinite(numeric) ? numeric : canonicalDeclarationText(initializer)}`;
    });
    return `enum{${members.join('|')}}`;
  }
  if (ts.isFunctionDeclaration(declaration)) {
    const parameters = declaration.parameters
      .map((parameter) => canonicalDeclarationText(parameter.getText()))
      .join(',');
    return `function<${typeParameters}>(${parameters}):${canonicalDeclarationText(declaration.type?.getText() ?? 'void')}`;
  }
  if (ts.isVariableDeclaration(declaration)) {
    return `variable:${canonicalDeclarationText(declaration.getText())}`;
  }
  return `type<${typeParameters}>=${canonicalDeclarationText(declaration.type.getText())}`;
}

function canonicalDeclarationText(value) {
  return value
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/\/\/.*$/gm, '')
    .replace(/\b(?:export|declare)\s+/g, '')
    .replace(/\s+/g, '')
    .replace(/,(?=\))/g, '')
    .replace(/;/g, '')
    .replace(/'/g, '"')
    .replace(/^\|/, '')
    .replace(/\(\|/g, '(');
}

function markdownLinks(content) {
  return [...content.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)]
    .map((match) => match[1])
    .filter((link) => link.length > 0);
}

function isExternalOrRoot(link) {
  return /^(?:https?:|mailto:|#|\/)/.test(link);
}
