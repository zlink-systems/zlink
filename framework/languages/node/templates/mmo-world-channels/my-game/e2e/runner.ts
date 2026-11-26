import assert from 'node:assert/strict';
import { spawn, type ChildProcess } from 'node:child_process';
import { createWriteStream, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { createInterface } from 'node:readline';
import { EventEmitter } from 'node:events';
import { finished } from 'node:stream/promises';
import { inspect } from 'node:util';
import { checkStructure } from './structure';
import {
  attachedScenarios,
  captureMessageFlow,
  keepRunning,
  reportFailure,
  scenarioStart,
  type Endpoints,
  type Foreground
} from './run-support';
import { runDomainChecks } from './domain-checks';
import {
  Limits,
  Scenarios,
  Control,
  NodeEntrypoints,
  type NodeRole
} from '../server/shared/constants';
import type { NodeConfig } from '../server/shared/config';

// Scenario failures are nested AggregateErrors; print every level so the cause reaches the CI log (#5312).
inspect.defaultOptions.depth = null;

const markers = Object.values(Control);

class NodeProcess {
  readonly child: ChildProcess;
  private readonly out: ReturnType<typeof createWriteStream>;
  private readonly lines: string[] = [];
  private lastLine: { at: string; line: string } | undefined;
  private readonly changed = new EventEmitter();
  private exited = false;
  readonly exit: Promise<Error | undefined>;
  constructor(
    readonly config: NodeConfig,
    readonly role: NodeRole,
    command: string,
    args: string[],
    logDir: string,
    sequence: number,
    foreground: boolean
  ) {
    this.out = createWriteStream(`${logDir}/${config.id}-${sequence}.out.log`);
    const err = createWriteStream(`${logDir}/${config.id}-${sequence}.err.log`);
    this.child = spawn(command, args, {
      detached: foreground,
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe']
    });
    console.log(`[${config.id}] Starting`);
    this.child.once('spawn', () => this.logDiagnostic('process-spawn'));
    const flow = captureMessageFlow(
      this.child.stderr!,
      `${logDir}/${config.id}-${sequence}.flow.log`,
      config.id,
      foreground
    );
    // The exit promise reports a log failure after all streams close.
    void flow.catch(() => {});
    this.child.stdout!.pipe(this.out);
    this.child.stderr!.pipe(err);
    const reader = createInterface({ input: this.child.stdout! });
    reader.on('line', (line) => {
      const at = new Date().toISOString();
      if (markers.some((marker) => line.startsWith(marker))) this.lines.push(line);
      else if (foreground) console.log(`[${config.id}] ${line}`);
      this.lastLine = { at, line };
      if (line.startsWith(Control.ready)) {
        this.logDiagnostic('ready-line-received', { line }, at);
        const { endpoint } = JSON.parse(line.slice(Control.ready.length)) as { endpoint: string };
        console.log(`[${config.id}] Ready ${endpoint}`);
      }
      this.changed.emit('change');
    });
    this.exit = new Promise<Error | undefined>((resolve) => {
      this.child.once('error', resolve);
      this.child.once('close', (code, signal) => {
        this.exited = true;
        this.changed.emit('change');
        reader.close();
        console.log(`[${config.id}] Stopped (exit=${code ?? signal})`);
        Promise.all([finished(this.out), finished(err), flow]).then(
          () =>
            code === 0
              ? resolve(undefined)
              : resolve(new Error(`${config.id} exit=${code} signal=${signal}; logs=${logDir}`)),
          (error: Error) => resolve(error)
        );
      });
    });
  }
  private logDiagnostic(
    event: string,
    details: Record<string, string | null> = {},
    at = new Date().toISOString()
  ): void {
    this.out.write(
      `ZLINK_E2E_DIAG ${JSON.stringify({ at, event, node: this.config.id, ...details })}\n`
    );
  }
  async wait(prefix: string, timeoutMs: number): Promise<string> {
    if (prefix.startsWith(Control.ready)) this.logDiagnostic('ready-wait-start');
    const found = this.lines.find((line) => line.startsWith(prefix));
    if (found) return found;
    return new Promise<string>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.changed.off('change', check);
      };
      const check = (): void => {
        const line = this.lines.find((l) => l.startsWith(prefix));
        if (line) {
          cleanup();
          resolve(line);
        } else if (this.exited) {
          cleanup();
          reject(new Error(`${this.config.id} exited before ${prefix}`));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        if (prefix.startsWith(Control.ready))
          this.logDiagnostic('ready-timeout', {
            lastLineAt: this.lastLine?.at ?? null,
            lastLine: this.lastLine?.line ?? null
          });
        reject(new Error(`${this.config.id} timeout waiting for ${prefix}`));
      }, timeoutMs);
      this.changed.on('change', check);
      check();
    });
  }
  async ready(): Promise<{ id: string; endpoint: string }> {
    const line = await this.wait(Control.ready, Limits.startupBudgetMs + Limits.shutdownMs);
    const ready = JSON.parse(line.slice(Control.ready.length)) as { id: string; endpoint: string };
    assert.equal(ready.id, this.config.id);
    assert(!ready.endpoint.endsWith(':0'), 'Unbound endpoint');
    return ready;
  }
  async drain(): Promise<void> {
    this.child.stdin!.write(Control.drain + '\n');
    await this.wait(Control.requested, Limits.messageMs);
    await this.wait(Control.relocated, Limits.relocationMs);
    await this.wait(Control.safe, Limits.safeMs);
  }
  async stop(): Promise<void> {
    if (!this.exited) this.child.stdin!.end(Control.shutdown + '\n');
    const timer = setTimeout(() => {
      if (!this.exited) this.child.kill();
    }, Limits.shutdownMs + Limits.messageMs);
    try {
      const failure = await this.exit;
      if (failure) throw failure;
      assert(
        this.lines.some((l) => l.startsWith(Control.stopped)),
        'Missing Stopped confirmation'
      );
    } finally {
      clearTimeout(timer);
    }
  }
}
async function main(): Promise<void> {
  const args = process.argv.slice(2);
  if (args[0] === '--check-structure') {
    assert(args.length <= 2);
    checkStructure(args[1]);
    return;
  }
  if (args[0] === '--check-domain') {
    assert.equal(args.length, 1);
    const checks = runDomainChecks();
    console.log(`Domain checks: ${checks}/${checks} passed`);
    return;
  }
  const selected = selectScenarios(args);
  checkStructure();
  const domainChecks = runDomainChecks();
  console.log(`Domain checks: ${domainChecks}/${domainChecks} passed`);
  let passed = 0;
  for (const name of selected) {
    scenarioStart(name, selected);
    const runId = randomUUID();
    const logDir = `.logs/run-${runId}`;
    mkdirSync(logDir, { recursive: true });
    if (await runCluster(name, runId, logDir)) {
      console.log(`✓ ${name}`);
      passed++;
    } else {
      for (const skipped of selected.slice(passed + 1))
        console.log(`Skipping ${skipped}: a prior scenario failed.`);
      break;
    }
  }
  console.log(`Scenarios: ${passed}/${selected.length} passed`);
  process.exitCode = passed === selected.length ? 0 : 1;
}

async function runCluster(
  name: string | undefined,
  runId: string,
  logDir: string,
  foreground?: Foreground
): Promise<boolean> {
  const project = `my-game-${runId}`;
  const dockerProject = foreground ? undefined : process.env.MY_GAME_DOCKER_PROJECT;
  const containers: string[] = [];
  const nodes = new Map<string, NodeProcess>();
  const all: NodeProcess[] = [];
  let failed = false;
  async function docker(args: string[], label: string): Promise<string> {
    const log = createWriteStream(`${logDir}/docker-${label}.log`);
    const child = spawn('docker', args, {
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let output = '';
    child.stdout.on('data', (chunk: Buffer) => {
      output += chunk.toString();
    });
    child.stdout.pipe(log, { end: false });
    child.stderr.pipe(log, { end: false });
    const code = await new Promise<number | null>((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    });
    log.end();
    await finished(log);
    assert.equal(code, 0, `Docker ${label}; log=${logDir}`);
    return output.trim();
  }
  function compose(args: string[], label: string): Promise<string> {
    return docker(['compose', '-p', project, '-f', 'docker-compose.yml', ...args], label);
  }
  async function start(config: NodeConfig, role: NodeRole): Promise<NodeProcess> {
    const path = `${logDir}/${config.id}.json`;
    writeFileSync(path, JSON.stringify(config, null, 2));
    const entrypoint = `dist/${NodeEntrypoints[role].replace(/\.ts$/, '.js')}`;
    let command = process.execPath;
    let args = ['--require', './dist/server/shared/diagnostics.js', entrypoint, path];
    if (dockerProject) {
      const container = `${dockerProject}-${config.id.toLowerCase()}-${runId}-${all.length}`;
      const image = process.env.MY_GAME_NODE_IMAGE;
      const runner = process.env.MY_GAME_RUNNER_CONTAINER;
      assert(image && runner, 'Missing Docker node image or runner container');
      await docker(
        [
          'create',
          '-i',
          '--name',
          container,
          '--label',
          `my-game.e2e=${dockerProject}`,
          '--network',
          `container:${runner}`,
          image,
          ...args.slice(0, -1),
          '/template/node.json'
        ],
        `${config.id}-create-${all.length}`
      );
      containers.push(container);
      await docker(
        ['cp', path, `${container}:/template/node.json`],
        `${config.id}-config-${all.length}`
      );
      command = 'docker';
      args = ['start', '-a', '-i', container];
    }
    const node = new NodeProcess(config, role, command, args, logDir, all.length, !!foreground);
    all.push(node);
    nodes.set(config.id, node);
    return node;
  }
  try {
    if (!dockerProject) {
      console.log('[redis] Starting');
      await compose(['up', '-d', '--wait'], 'up');
      console.log('[redis] Ready');
    }
    foreground?.signal.throwIfAborted();
    const redis = new Map<string, string>();
    for (const world of ['A', 'B'])
      redis.set(
        world,
        dockerProject
          ? `db${world}:6379`
          : await compose(['port', 'db' + world, '6379'], 'port-' + world)
      );
    const endpoints: Record<string, string> = {};
    const structure = JSON.parse(readFileSync('zlink.structure.json', 'utf8')) as {
      nodes: { role: string; config: string | null }[];
    };
    const applicationNodes = structure.nodes
      .filter((n) => n.role === 'channel' || n.role === 'session' || n.role === 'accounts')
      .map((node) => {
        assert(node.config);
        return {
          role: node.role as NodeRole,
          config: JSON.parse(readFileSync(node.config, 'utf8')) as NodeConfig
        };
      });
    const order = { channel: 0, session: 1, accounts: 2 };
    applicationNodes.sort((a, b) => order[a.role] - order[b.role]);
    for (const node of applicationNodes) {
      foreground?.signal.throwIfAborted();
      const cfg = node.config;
      cfg.redisEndpoint = redis.get(cfg.worldId)!;
      cfg.keyPrefix = `my-game:${runId}:world:${cfg.worldId}:`;
      cfg.meshEndpoint = 'tcp://127.0.0.1:0';
      if (node.role === 'session') cfg.streamEndpoint = 'ws://127.0.0.1:0';
      if (node.role === 'accounts') {
        cfg.httpEndpoint = 'http://127.0.0.1:0';
        cfg.worlds = cfg.worlds!.map((w) => ({ ...w, sessionEndpoint: endpoints[w.worldId] }));
      }
      const started = await start(cfg, node.role);
      const ready = await started.ready();
      if (node.role === 'session') endpoints[cfg.worldId] = ready.endpoint;
    }
    const accounts = [...nodes.values()].find((n) => n.role === 'accounts');
    assert(accounts);
    const accountsEndpoint = (await accounts.ready()).endpoint;
    if (foreground) {
      await keepRunning(
        foreground,
        { stream: Object.values(endpoints)[0], worlds: endpoints, api: accountsEndpoint },
        logDir,
        all.map((node) => node.exit.then((error) => error ?? new Error(`${node.config.id} exited`)))
      );
    } else {
      assert(name);
      const { scenario } = await import('./client.mjs');
      const control = {
        async drain(id: string): Promise<void> {
          const node = nodes.get(id);
          assert(node, `Unknown host ${id}`);
          await node.drain();
        },
        async createUpdateZone(id: string): Promise<void> {
          const node = nodes.get(id);
          assert(node);
          node.child.stdin!.write(Control.createUpdateZone + '\n');
          await node.wait(Control.updateZoneCreated, Limits.messageMs);
        },
        async replace(id: string): Promise<void> {
          const node = nodes.get(id);
          assert(node);
          await node.stop();
          const replacement = await start({ ...node.config }, node.role);
          await replacement.ready();
        }
      };
      await scenario(name, endpoints, accountsEndpoint, runId, logDir, control);
    }
  } catch (error) {
    if (!foreground || error !== foreground.signal.reason) {
      failed = true;
      reportFailure(foreground ? 'Server failed' : `✗ ${name}`, error, logDir, name ?? 'startup');
    }
  } finally {
    for (const node of all.reverse())
      try {
        await node.stop();
      } catch (error) {
        failed = true;
        reportFailure('Node cleanup failed', error, logDir, node.config.id + '-cleanup');
      }
    try {
      if (!dockerProject) await compose(['logs', '--no-color'], 'logs');
    } catch (error) {
      failed = true;
      reportFailure('Redis log collection failed', error, logDir, 'redis-logs');
    }
    for (const container of containers) {
      try {
        await docker(['rm', '-f', container], `${container}-remove`);
      } catch (error) {
        failed = true;
        reportFailure('Container cleanup failed', error, logDir, container + '-cleanup');
      }
    }
    try {
      if (!dockerProject) {
        await compose(['down', '--volumes', '--remove-orphans'], 'down');
        console.log('[redis] Stopped');
      }
    } catch (error) {
      failed = true;
      reportFailure('Redis cleanup failed', error, logDir, 'redis-cleanup');
    }
  }
  if (!foreground || failed) console.log(`Logs: ${logDir}`);
  return !failed;
}
function selectScenarios(args: string[]): readonly string[] {
  let selected: readonly string[] = Scenarios;
  if (args.length) {
    assert.equal(args.length, 2);
    assert.equal(args[0], '--scenario');
    const requested = args[1].split(',');
    assert.equal(new Set(requested).size, requested.length, 'Duplicate scenario');
    for (const name of requested)
      assert(Scenarios.includes(name as (typeof Scenarios)[number]), `Unknown scenario ${name}`);
    selected = Scenarios.filter((s) => requested.includes(s));
  }
  return selected;
}

export async function serve(
  runId: string,
  logDir: string,
  signal: AbortSignal,
  publish: Foreground['publish']
): Promise<void> {
  checkStructure();
  process.exitCode = (await runCluster(undefined, runId, logDir, { signal, publish })) ? 0 : 1;
}

export async function attach(args: string[], endpoints: Endpoints): Promise<void> {
  const selected = attachedScenarios(selectScenarios(args), ['update']);
  const runId = randomUUID();
  const logDir = `.logs/run-${runId}`;
  mkdirSync(logDir, { recursive: true });
  console.log(`Logs: ${logDir}`);
  if (!selected.length) {
    console.log('Scenarios: 0/0 passed');
    return;
  }
  assert(endpoints.worlds && endpoints.api, 'Missing world or Accounts endpoint');
  const { scenario } = await import('./client.mjs');
  let passed = 0;
  for (const name of selected) {
    scenarioStart(name, selected);
    const scenarioLog = `${logDir}/${name}`;
    mkdirSync(scenarioLog);
    try {
      await scenario(name, endpoints.worlds, endpoints.api, runId, scenarioLog);
      console.log(`✓ ${name}`);
      passed++;
    } catch (error) {
      reportFailure(`✗ ${name}`, error, logDir, name);
    }
  }
  console.log(`Scenarios: ${passed}/${selected.length} passed`);
  process.exitCode = passed === selected.length ? 0 : 1;
}

if (require.main === module) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
