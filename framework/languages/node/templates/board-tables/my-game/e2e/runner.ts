import assert from 'node:assert/strict';
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import {
  createWriteStream,
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync
} from 'node:fs';
import { randomUUID } from 'node:crypto';
import { createInterface } from 'node:readline';
import { EventEmitter } from 'node:events';
import { finished } from 'node:stream/promises';
import { inspect } from 'node:util';
import { checkStructure, isNodeRole } from './structure';
import {
  Limits,
  Scenarios,
  Control,
  NodeEntrypoints,
  type NodeRole
} from '../server/shared/constants';
import type { NodeControl } from './client.mjs' with { 'resolution-mode': 'import' };
import type { NodeConfig } from '../server/shared/config';

function runDomainChecks(): void {
  execFileSync(process.execPath, ['--test', 'dist/e2e/domain.test.js'], { stdio: 'inherit' });
  console.log('Domain checks: 7/7');
}
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
    this.child.once('spawn', () => this.logDiagnostic('process-spawn'));
    this.child.stdout!.pipe(this.out);
    this.child.stderr!.pipe(err);
    const reader = createInterface({ input: this.child.stdout! });
    reader.on('line', (line) => {
      const at = new Date().toISOString();
      this.lines.push(line);
      this.lastLine = { at, line };
      if (line.startsWith(Control.ready)) this.logDiagnostic('ready-line-received', { line }, at);
      this.changed.emit('change');
    });
    this.exit = new Promise<Error | undefined>((resolve) => {
      this.child.once('error', resolve);
      this.child.once('close', (code, signal) => {
        this.exited = true;
        this.changed.emit('change');
        reader.close();
        Promise.all([finished(this.out), finished(err)]).then(
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
    return JSON.parse(line.slice(Control.ready.length)) as {
      id: string;
      endpoint: string;
    };
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
    } finally {
      clearTimeout(timer);
    }
  }
}
const serverFile = '.logs/server.json';

interface ServerState {
  pid: number;
  endpoint?: string;
}

function runningServer(): ServerState | undefined {
  try {
    const state = JSON.parse(readFileSync(serverFile, 'utf8')) as ServerState;
    assert(Number.isSafeInteger(state.pid) && state.pid > 0, 'Invalid server PID');
    process.kill(state.pid, 0);
    return state;
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (code === 'ENOENT' || code === 'ESRCH') return undefined;
    throw error;
  }
}

async function runScenarios(
  selected: readonly string[],
  endpoint: string,
  runId: string,
  logDir: string,
  control?: NodeControl
): Promise<boolean> {
  const { scenario } = await import('./client.mjs');
  let passed = 0;
  for (const [index, name] of selected.entries()) {
    console.log(`━━ ${name} (${index + 1}/${selected.length}) ━━`);
    try {
      await scenario(name, endpoint, runId, logDir, control);
      passed++;
      console.log(`✓ ${name}`);
    } catch (error) {
      const path = `${logDir}/${name}-failure.log`;
      writeFileSync(path, inspect(error, { depth: null }) + '\n');
      const message = error instanceof Error ? error.message.split('\n')[0] : String(error);
      console.error(`✗ ${name}: ${message} (${path})`);
    }
  }
  console.log(`Scenarios: ${passed}/${selected.length} passed`);
  return passed === selected.length;
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  if (args[0] === '--domain-checks-only') {
    assert.equal(args.length, 1);
    runDomainChecks();
    return;
  }
  if (args[0] === '--check-structure') {
    assert(args.length <= 2);
    checkStructure(args[1]);
    return;
  }
  if (args[0] === '--server-running' || args[0] === '--server-active') {
    assert.equal(args.length, 1);
    const server = runningServer();
    process.exitCode = (args[0] === '--server-running' ? server?.endpoint : server) ? 0 : 1;
    return;
  }
  const foreground = args[0] === '--serve';
  const attach = args[0] === '--attach';
  if (foreground || attach) args.shift();
  if (foreground) assert.equal(args.length, 0);
  const isolated = (name: string): boolean => name === 'update' || name === 'persist';
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
  checkStructure();
  if (!foreground && !attach) runDomainChecks();
  const runId = randomUUID();
  const logDir = `.logs/run-${runId}`;
  mkdirSync(logDir, { recursive: true });
  if (attach) {
    const server = runningServer();
    assert(server?.endpoint, 'Start the server with run-server and wait for its STREAM endpoint.');
    if (args.length === 0) {
      selected = selected.filter((name) => !isolated(name));
      console.log(
        'Running against the started server; update and persist require an isolated run.'
      );
    }
    assert(!selected.some(isolated), 'The update and persist scenarios require an isolated run.');
    console.log(`Attached to Session STREAM: ${server.endpoint}`);
    console.log(`Logs: ${logDir}`);
    process.exitCode = (await runScenarios(selected, server.endpoint, runId, logDir)) ? 0 : 1;
    return;
  }

  const project = `my-game-${runId}`;
  const dockerProject = process.env.MY_GAME_DOCKER_PROJECT;
  const containers: string[] = [];
  const nodes = new Map<string, NodeProcess>();
  const all: NodeProcess[] = [];
  let failed = false;
  const abort = new AbortController();
  const interrupt = (): void => abort.abort();
  const stopping = new Promise<void>((resolve) => {
    abort.signal.addEventListener('abort', () => resolve(), { once: true });
  });
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
    let args = [entrypoint, path];
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
          entrypoint,
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
    const node = new NodeProcess(config, role, command, args, logDir, all.length, foreground);
    all.push(node);
    nodes.set(config.id, node);
    return node;
  }
  if (foreground) {
    if (existsSync(serverFile)) {
      assert(!runningServer(), 'A server is already starting or running. Stop it with Ctrl+C.');
      rmSync(serverFile, { force: true });
    }
    writeFileSync(serverFile, JSON.stringify({ pid: process.pid }), { flag: 'wx' });
    process.on('SIGINT', interrupt);
    process.on('SIGTERM', interrupt);
  }
  const pendingState = `${logDir}/server.json`;
  console.log(`Logs: ${logDir}`);
  try {
    if (!dockerProject) await compose(['up', '-d', '--wait'], 'up');
    if (abort.signal.aborted) return;
    const redis = dockerProject ? 'redis:6379' : await compose(['port', 'redis', '6379'], 'port');
    const structure = JSON.parse(readFileSync('zlink.structure.json', 'utf8')) as {
      nodes: { role: string; config: string | null }[];
    };
    for (const node of structure.nodes) {
      if (abort.signal.aborted) return;
      if (!isNodeRole(node.role)) continue;
      assert(node.config);
      const cfg = JSON.parse(readFileSync(node.config, 'utf8')) as NodeConfig;
      if (node.role === 'table' && !foreground)
        cfg.tableCapacity = selected.filter((name) => name !== 'persist').length;
      cfg.redisEndpoint = redis;
      cfg.keyPrefix = `my-game:${runId}:`;
      if (cfg.meshEndpoint !== null) cfg.meshEndpoint = 'tcp://127.0.0.1:0';
      await start(cfg, node.role);
    }
    const ready = await Promise.all([...nodes.values()].map((n) => n.ready()));
    const session = [...nodes.values()].find((n) => n.role === 'session');
    assert(session);
    const endpoint = ready.find((r) => r.id === session.config.id)!.endpoint;
    assert(endpoint.startsWith('ws://'), 'Session must publish a WebSocket endpoint.');
    if (foreground) {
      if (abort.signal.aborted) return;
      writeFileSync(pendingState, JSON.stringify({ pid: process.pid, endpoint }));
      renameSync(pendingState, serverFile);
      console.log(`Server ready: ${endpoint} (game clients connect here)`);
      const exited = await Promise.race([
        stopping.then(() => undefined),
        ...all.map((node) =>
          node.exit.then((error) => error ?? new Error(`${node.config.id} exited`))
        )
      ]);
      if (exited) throw exited;
      return;
    }
    const control = {
      async restartTables(): Promise<void> {
        const tables = [...nodes.values()].filter((node) => node.role === 'table');
        await Promise.all(tables.map((table) => table.stop()));
        const replacements = [];
        for (const table of tables) replacements.push(await start(table.config, table.role));
        await Promise.all(replacements.map((table) => table.ready()));
      },
      async drain(id: string): Promise<void> {
        const node = nodes.get(id);
        assert(node, `Unknown host ${id}`);
        await node.drain();
      },
      async replace(id: string): Promise<void> {
        const node = nodes.get(id);
        assert(node);
        await node.stop();
        const replacement = await start(node.config, node.role);
        await replacement.ready();
      }
    };
    failed = !(await runScenarios(selected, endpoint, runId, logDir, control));
  } catch (error) {
    failed = true;
    console.error('Server failed', error);
  } finally {
    for (const node of all.reverse())
      try {
        await node.stop();
      } catch (error) {
        failed = true;
        console.error('Node cleanup failed', error);
      }
    for (const container of containers) {
      try {
        await docker(['rm', '-f', container], `${container}-remove`);
      } catch (error) {
        failed = true;
        console.error('Container cleanup failed', error);
      }
    }
    try {
      if (!dockerProject) await compose(['down', '--volumes', '--remove-orphans'], 'down');
    } catch (error) {
      failed = true;
      console.error('Redis cleanup failed', error);
    }
    if (foreground) {
      rmSync(serverFile, { force: true });
      process.off('SIGINT', interrupt);
      process.off('SIGTERM', interrupt);
    }
    process.exitCode = failed ? 1 : 0;
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
