const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {createRequire} = require('node:module');
const assert = require('node:assert/strict');
const test = require('node:test');
const root = path.resolve(__dirname, '../..');
const req = createRequire(root + '/package.json');
const ts = req('typescript');
const spec = req('./samples/ZoneWorld/dist/Shared/spec.js');
const {BotIds, ZoneIds, NodeIds, ZoneWorldSpec} = spec;
// A NW client sees adjacent residents inside the server's border band (Server/ZoneNode/Domain/world.ts inBorderBand).
const bandEdge = ZoneWorldSpec.zoneSplit + ZoneWorldSpec.borderBand;
function loadRunBots(factory, output) {
  let source = fs.readFileSync(root + '/samples/ZoneWorld/Client/special.ts', 'utf8');
  source = source.slice(0, source.lastIndexOf('main().catch')) + '\nexports.runBots = runBots;';
  const code = ts.transpileModule(source, {compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022}}).outputText;
  const context = {exports: {}, console: {log: value => output.push(value)}, require: name => {
    if (name === '@zlink-systems/stream-connector') return {
      zlinkStreamConnectorFactory: factory, zlinkStreamDispatchMode: {Immediate: 0},
      ZlinkStreamDispatchMode: {Immediate: 0}, zlinkStreamAssert: {ensure: (condition, message) => {if (!condition) throw Error(message);}}
    };
    if (name === './join-readiness') return {joinAndWaitForOwnedState: async () => ({playerId: 'player-f', zoneId: 'zone-nw', x:25, y:25, error:null})};
    if (name === '../Server/Configuration/configuration') return {};
    if (name.startsWith('../Shared/')) return req('./samples/ZoneWorld/dist/Shared/' + path.basename(name) + '.js');
    return req(name);
  }};
  vm.runInNewContext(code, context);
  return context.exports.runBots;
}
async function simulate(zones, phase = 7000) {
  let now = phase;
  let scheduled = false;
  const waits = [];
  const markers = [];
  const maintenance = new Set();
  const enabled = [];
  const observations = [];
  const rejections = [];
  const bots = [
    {playerId: BotIds.northWestX, x:10,y:15,dx:1,dy:0},
    {playerId: BotIds.northWestY, x:15,y:10,dx:0,dy:1},
    {playerId: BotIds.northEastX, x:90,y:15,dx:-1,dy:0},
    {playerId: BotIds.northEastY, x:85,y:10,dx:0,dy:1},
    {playerId: BotIds.southWestX, x:10,y:85,dx:1,dy:0},
    {playerId: BotIds.southWestY, x:15,y:90,dx:0,dy:-1},
    {playerId: BotIds.southEastX, x:90,y:85,dx:-1,dy:0},
    {playerId: BotIds.southEastY, x:85,y:90,dx:0,dy:-1}
  ];
  const owner = zone => zones.findIndex(value => value.includes(zone)) === 0 ? NodeIds.west : NodeIds.east;
  const nodes = zones.map((value, index) => ({nodeId: index === 0 ? NodeIds.west : NodeIds.east, registered:true, connected:true, maintenance:maintenance.has(index === 0 ? NodeIds.west : NodeIds.east), zones:value}));
  function tickBots() {
    for (const bot of bots) {
      const x = bot.x + bot.dx * 3, y = bot.y + bot.dy * 3;
      if (x < 0 || x > 99 || y < 0 || y > 99 ||
          (spec.zoneOf(x, y) !== spec.zoneOf(bot.x, bot.y) && maintenance.has(owner(spec.zoneOf(x, y))))) {
        if (x >= 0 && x <= 99 && y >= 0 && y <= 99) rejections.push({time: now, playerId: bot.playerId, target: owner(spec.zoneOf(x, y))});
        bot.dx *= -1; bot.dy *= -1;
      } else { bot.x=x; bot.y=y; }
    }
  }
  for (let time=500; time<=phase; time+=500) tickBots();
  function pump() {
    scheduled = false;
    if (waits.length === 0) return;
    now += 100;
    if (now%500 === 0) tickBots();
    const players = bots.map(bot => ({playerId:bot.playerId, x:bot.x, y:bot.y, zoneId:spec.zoneOf(bot.x,bot.y), isBot:true}))
      .filter(bot => bot.zoneId === ZoneIds.northWest || (bot.zoneId === ZoneIds.northEast && bot.x < bandEdge) || (bot.zoneId === ZoneIds.southWest && bot.y < bandEdge));
    for (const wait of [...waits]) {
      if (!waits.includes(wait)) continue;
      const payload = wait.packet === 'ZoneStateNotify' ? {zoneId:ZoneIds.northWest,tick:now/100,players} :
        wait.packet === 'NodeStatusNotify' ? nodes.find(node => wait.predicate({payload: {...node, maintenance:maintenance.has(node.nodeId)}})) :
        wait.packet === 'MoveRejectedNotify' ? {reason:'OutOfRange'} : undefined;
      if (payload !== undefined) {
        const message = {payload: wait.packet === 'NodeStatusNotify' ? {...payload,maintenance:maintenance.has(payload.nodeId)} : payload};
        if (wait.predicate(message)) {
          waits.splice(waits.indexOf(wait),1);
          observations.push({time:now, packet:wait.packet,players:message.payload.players});
          wait.resolve(message);
          continue;
        }
      }
      if (now >= wait.deadline) {
        waits.splice(waits.indexOf(wait),1);
        wait.reject(Error('Observation timed out at ' + now + ': ' + wait.predicate));
      }
    }
    schedule();
  }
  function schedule() { if (!scheduled) {scheduled=true;setImmediate(pump);} }
  const factory = { create({endpoint}) {return {
    async connect(){}, async close(){},
    waitFor(packet) {
      let predicate=()=>true, timeout=60000;
      return {where(value){predicate=value;return this;},timeout(value){timeout=value;return this;},submit(){
        return new Promise((resolve,reject) => {waits.push({packet,predicate,deadline:now+timeout,resolve,reject});schedule();});
      }};
    },
    send(){return {packetName(){return this;}, async submit(){}};},
    request(message){return {packetName(){return this;}, async submit(){
      if (message.constructor.name === 'WatchNodesReq') return {nodes};
      if (message.constructor.name === 'SetMaintenanceReq') {
        if (message.enabled) {maintenance.add(message.nodeId);enabled.push({time:now,node:message.nodeId, rejectionsBefore: rejections.length});}
        else maintenance.delete(message.nodeId);
        return {nodeId:message.nodeId,enabled:message.enabled,error:null};
      }
      return {error:null};
    } };}
  };}};
  try {await loadRunBots(factory, markers)('game','ops');return {phase, zones, markers, enabled, observations, rejections, now};}
  catch(error){return {phase,zones,markers,enabled,observations,rejections,now,error:String(error)};}
}

// Replay the canonical 100 ms snapshots and 500 ms / three-cell bot patrol,
// with admission decided by the owner reported through WatchNodesRes. No wall
// clock or native transport is involved: this test owns only the client script.
for (const [name, zones] of [
  ['north/south', [['zone-ne', 'zone-nw'], ['zone-se', 'zone-sw']]],
  ['west/east', [['zone-sw', 'zone-nw'], ['zone-se', 'zone-ne']]],
  ['diagonal', [['zone-nw', 'zone-se'], ['zone-ne', 'zone-sw']]]
]) {
  test(`ZoneWorld bot reversal targets the observed destination owner (${name})`, async () => {
    const result = await simulate(zones);
    assert.equal(result.error, undefined);
    assert.equal(result.enabled.length, 1);
    const enabled = result.enabled[0];
    const boundary = result.observations.filter(value => value.packet === 'ZoneStateNotify'
      && value.time <= enabled.time).at(-1);
    assert.ok(boundary, 'maintenance is applied to an observed boundary bot, after the movement check');
    const candidates = boundary.players.filter(bot => bot.playerId.endsWith('-x')
      && ((bot.zoneId === 'zone-nw' && bot.x + 3 >= 50)
        || (bot.zoneId === 'zone-ne' && bot.x - 3 < 50)));
    assert.ok(candidates.length > 0, 'the selected bot is at the X boundary');
    const candidate = candidates[0];
    const destination = candidate.zoneId === 'zone-nw' ? 'zone-ne' : 'zone-nw';
    const owner = zones[0].includes(destination) ? NodeIds.west : NodeIds.east;
    assert.equal(enabled.node, owner, 'NodeId labels do not determine zone ownership');
    assert.ok(result.rejections.slice(enabled.rejectionsBefore).some(rejection =>
      rejection.playerId === candidate.playerId && rejection.target === owner),
    'the selected bot must actually be refused by the maintained owner');
    assert.deepEqual(result.markers, ['scenario ZW-F1 passed', 'scenario ZW-F3 passed', 'scenario ZW-F4 passed']);
    assert.ok(result.now - result.phase < 60_000, 'the original runner budget contains the entire bot scenario');
  });
}

test('ZoneWorld bots never access a bound session for state, announcement, or rejection pushes', () => {
  const { PlayerActor } = req('./samples/ZoneWorld/dist/Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.js');
  const { ZoneStateNotify, WorldAnnounceNotify, MoveRejectedNotify } = req('./samples/ZoneWorld/dist/Shared/contracts.js');
  const bot = new PlayerActor(BotIds.northEastX, 48, 15, ZoneIds.northWest, true, 1, 0);
  Object.defineProperty(bot, 'context', {get() {throw new Error('an unbound bot accessed its session');}});
  for (const message of [new ZoneStateNotify(ZoneIds.northWest, 1, []),
    new WorldAnnounceNotify('bot-test', 'hello'), new MoveRejectedNotify('ZoneMaintenance', 48, 15)]) {
    assert.doesNotThrow(() => bot.push(message));
  }
});
