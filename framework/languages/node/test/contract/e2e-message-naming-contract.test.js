const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const root = path.resolve(__dirname, '../..');
const e2eRoot = path.join(root, 'e2e');

function sourceFiles(directory = e2eRoot) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) return sourceFiles(absolute);
    return entry.isFile() && entry.name.endsWith('.ts') ? [absolute] : [];
  });
}

function read(relative) {
  return fs.readFileSync(path.join(root, relative), 'utf8');
}

test('E2E message declarations use Req, Res, Msg, Notify, or Event suffixes', () => {
  const violations = [];
  for (const file of sourceFiles()) {
    const source = ts.createSourceFile(file, fs.readFileSync(file, 'utf8'), ts.ScriptTarget.Latest, true);
    const visit = (node) => {
      if ((ts.isClassDeclaration(node)
          || ts.isInterfaceDeclaration(node)
          || ts.isTypeAliasDeclaration(node)
          || ts.isEnumDeclaration(node))
        && node.name !== undefined
        && /(?:Request|Reply|Response|Command|Result|Ack)$/.test(node.name.text)) {
        const line = source.getLineAndCharacterOfPosition(node.name.getStart(source)).line + 1;
        violations.push(`${path.relative(root, file)}:${line}:${node.name.text}`);
      }
      ts.forEachChild(node, visit);
    };
    visit(source);
  }
  assert.deepEqual(violations, []);
});

test('publish packets use Event names at declarations, registrations, and call sites', () => {
  const pubSubMessages = read('e2e/PubSub/Shared/messages.ts');
  const pubSubHost = read('e2e/PubSub/Server/Subscriber/subscriber-host-factory.ts');
  const pubSubPublisher = read('e2e/PubSub/Server/Publisher/Endpoints/publisher-endpoints.ts');
  const observabilityMessages = read('e2e/ObservabilityOps/Shared/messages.ts');
  const observabilityWorkflow = read('e2e/ObservabilityOps/Server/Workflow/main.ts');
  const monitoringMessages = read('e2e/RuntimeMonitoring/Shared/messages.ts');
  const monitoringEndpoints = read('e2e/RuntimeMonitoring/Server/Service/Endpoints/service-endpoints.ts');

  assert.match(pubSubMessages, /class PubSubEvent\b/);
  assert.match(pubSubMessages, /pubSubEvent: 'PubSubEvent'/);
  assert.match(pubSubHost, /addPublishHandler\(PacketNames\.pubSubEvent, PubSubEventHandler\)/);
  assert.match(pubSubPublisher, /new PubSubEvent\([\s\S]*?\.publish\([\s\S]*?event\)\.submit\(\)/);
  assert.doesNotMatch(`${pubSubMessages}\n${pubSubHost}\n${pubSubPublisher}`, /EventMsg/);

  assert.match(observabilityMessages, /class WorkflowProjectedEvent\b/);
  assert.match(observabilityWorkflow, /publish\([\s\S]*?new WorkflowProjectedEvent\(/);
  assert.match(monitoringMessages, /class MonitoringEvent\b/);
  assert.match(monitoringEndpoints, /publish\([\s\S]*?new MonitoringEvent\(/);
});

test('one-way E2E send paths use Msg payloads and client pushes use Notify payloads', () => {
  const automaticTurn = read('e2e/AutomaticTurnDispatch/Server/Play/Handlers/execution-turn-handlers.ts');
  const transfer = read('e2e/SpotActorTransfer/Server/ActorNode/main.ts');
  const registry = read('e2e/RegistryMessaging/Server/ObjectClient/Endpoints/object-client-endpoints.ts');
  const toActorMessages = read('e2e/ToActorMessaging/Shared/messages.ts');
  const toActorServer = read('e2e/ToActorMessaging/Server/Actor/main.ts');

  assert.match(automaticTurn, /sendToSpot\([\s\S]*?new ProbeMsg\(/);
  assert.doesNotMatch(automaticTurn, /sendToSpot\([\s\S]{0,160}?new ProbeReq\(/);
  assert.match(transfer, /sendToActor\([\s\S]*?new HandoffProbeMsg\(/);
  assert.doesNotMatch(transfer, /sendToActor\([\s\S]{0,160}?new (?:HandoffProbe|Probe)Req\(/);
  assert.match(registry, /sendToNode\([\s\S]*?new ScenarioRouteMsg\(/);
  assert.doesNotMatch(registry, /sendToNode\([\s\S]{0,160}?new ScenarioRouteReq\(/);

  assert.match(toActorMessages, /class ActorMsg\b/);
  assert.match(toActorMessages, /class ActorPushNotify\b/);
  assert.match(toActorServer, /boundSession[\s\S]*?\.send\(new ActorPushNotify\(/);
});

test('RouteMesh actor and spot create payloads use named Req wrappers', () => {
  const monitoring = read('e2e/RuntimeMonitoring/Server/Service/Endpoints/service-endpoints.ts');
  const discovery = read('e2e/DiscoveryRegistryHa/Server/Provider/Endpoints/provider-endpoints.ts');
  const automaticTurn = read('e2e/AutomaticTurnDispatch/Server/Play/Handlers/control-handlers.ts');
  const spotService = read('e2e/SpotService/Server/MultiNode/Endpoints/multi-node-endpoints.ts');

  assert.match(monitoring, /\.create\(MonitoringUserSpot\.name\)[\s\S]*?\.request\(new MonitoringSpotCreateReq\(\)\)/);
  assert.match(monitoring, /\.create\(actorId,[\s\S]*?\.request\(new MonitoringActorCreateReq\(\)\)/);
  assert.match(discovery, /actors\.create\([\s\S]*?\.request\(new Config6ActorCreateReq\(/);
  assert.match(discovery, /spots\.getOrCreate\([\s\S]*?\.request\(new Config6UserSpotCreateReq\(/);
  assert.match(automaticTurn, /actors\.getOrCreate\([\s\S]*?\.request\(new AwaitActorCreateReq\(/);
  assert.match(spotService, /actors[\s\S]*?\.getOrCreate\([\s\S]*?\.request\(new ScenarioActorCreateReq\(/);
  assert.doesNotMatch(`${monitoring}\n${discovery}\n${automaticTurn}\n${spotService}`,
    /\.(?:create|getOrCreate)\([\s\S]{0,300}?\.request\(\s*(?:\{|ZLinkMessage\.from\()/);
});
