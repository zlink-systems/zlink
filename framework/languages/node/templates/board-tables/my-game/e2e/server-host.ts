import assert from 'node:assert/strict';
import { spawn, type ChildProcess } from 'node:child_process';
import { createWriteStream, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { createInterface } from 'node:readline';
import { EventEmitter } from 'node:events';
import { finished } from 'node:stream/promises';
import { readConfig, type NodeConfig } from '../server/shared/config';
import {
  Limits,
  Scenarios,
  Control,
  NodeEntrypoints,
  LogFiles,
  LocalHost,
  ServerLog,
  type NodeRole
} from '../server/shared/constants';
import { checkStructure } from './structure';
import {
  assertNoRunningServer,
  publishServer,
  ServerAlreadyRunningError,
  type NodeControl,
  type ServerState
} from './server-control';

// What a standalone server prints. Each line is "<time>  [<server id>]  <message>". Server lines,
// warnings and errors always appear; diagnostic records (message flow) appear only with trace. Every
// record stays in the node's file log as JSON.
export interface ConsoleOptions {
  trace: boolean;
}
function now(): string {
  return new Date().toISOString().slice(LogFiles.consoleTimeStart, LogFiles.consoleTimeEnd);
}
// Control lines are for the host; the node prints a readable server line for each.
const controlEchoes = [
  Control.ready,
  Control.stopped,
  Control.requested,
  Control.relocated,
  Control.safe
] as const;
const peerLine = new RegExp(`(${ServerLog.connectedTo}|${ServerLog.disconnectedFrom}) (\\S+)$`);
function diagnosticRecord(line: string): { time: string; body: string } | undefined {
  try {
    const record = JSON.parse(line) as { time?: unknown; body?: unknown };
    if (typeof record.time === 'string' && typeof record.body === 'string')
      return { time: record.time, body: record.body };
  } catch {
    // Not a diagnostic record.
  }
  return undefined;
}
class ServerConsole {
  private readonly titles = new Map<string, string>();
  private width = LogFiles.hostTag.length;
  constructor(private readonly options: ConsoleOptions) {}
  addServer(id: string, title: string | undefined): void {
    if (title) this.titles.set(id, title);
    this.width = Math.max(this.width, id.length);
  }
  host(message: string): void {
    this.write(console.log, LogFiles.hostTag, now(), message);
  }
  stdout(id: string, line: string): void {
    if (controlEchoes.some((prefix) => line.startsWith(prefix))) return;
    // "connected to playA" reads as "connected to Play server A".
    const text = line.replace(peerLine, (_, verb: string, peer: string) => {
      return `${verb} ${this.titles.get(peer) ?? peer}`;
    });
    this.write(console.log, id, now(), text);
  }
  stderr(id: string, line: string): void {
    const record = diagnosticRecord(line);
    if (!record) this.write(console.error, id, now(), line);
    else if (this.options.trace)
      this.write(
        console.error,
        id,
        record.time.slice(LogFiles.consoleTimeStart, LogFiles.consoleTimeEnd),
        record.body
      );
  }
  private write(out: (line: string) => void, id: string, time: string, message: string): void {
    out(`${time}  ${`[${id}]`.padEnd(this.width + 2)}  ${message}`);
  }
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
    serverConsole: ServerConsole | undefined
  ) {
    this.out = createWriteStream(`${logDir}/${config.id}-${sequence}.out.log`);
    const err = createWriteStream(`${logDir}/${config.id}-${sequence}.err.log`);
    this.child = spawn(command, args, {
      detached: serverConsole !== undefined,
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe']
    });
    this.child.stdin!.on('error', (error: Error) => {
      this.logDiagnostic('stdin-error', { message: error.message });
    });
    this.child.once('spawn', () => this.logDiagnostic('process-spawn'));
    this.child.stdout!.pipe(this.out);
    this.child.stderr!.pipe(err);
    if (serverConsole) {
      createInterface({ input: this.child.stderr! }).on('line', (line) => {
        serverConsole.stderr(config.id, line);
      });
    }
    const reader = createInterface({ input: this.child.stdout! });
    reader.on('line', (line) => {
      serverConsole?.stdout(config.id, line);
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
  async wait(prefix: string, timeoutMs: number, signal?: AbortSignal): Promise<string> {
    signal?.throwIfAborted();
    if (prefix.startsWith(Control.ready)) this.logDiagnostic('ready-wait-start');
    const found = this.lines.find((line) => line.startsWith(prefix));
    if (found) return found;
    return new Promise<string>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.changed.off('change', check);
        signal?.removeEventListener('abort', abort);
      };
      const abort = (): void => {
        cleanup();
        reject(signal?.reason);
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
      signal?.addEventListener('abort', abort, { once: true });
      check();
    });
  }
  async ready(signal?: AbortSignal): Promise<{ id: string; endpoint: string }> {
    return JSON.parse(
      (await this.wait(Control.ready, Limits.startupMs, signal)).slice(Control.ready.length)
    ) as {
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
export function createServerHost(tableCapacity: number, consoleOptions?: ConsoleOptions) {
  const serverConsole = consoleOptions ? new ServerConsole(consoleOptions) : undefined;
  const runId = randomUUID();
  const keyPrefix = `${LocalHost.keyPrefix}${runId}:`;
  const logDir = `${LogFiles.runs}/${LogFiles.runPrefix}${runId}`;
  mkdirSync(logDir, { recursive: true });
  const project = `${LocalHost.projectPrefix}${runId}`;
  const dockerProject = process.env.MY_GAME_DOCKER_PROJECT;
  const containers: string[] = [];
  const nodes = new Map<string, NodeProcess>();
  const all: NodeProcess[] = [];
  async function docker(args: string[], label: string, signal?: AbortSignal): Promise<string> {
    const log = createWriteStream(`${logDir}/docker-${label}.log`);
    const child = spawn('docker', args, {
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe'],
      signal
    });
    let output = '';
    child.stdout.on('data', (chunk: Buffer) => {
      output += chunk.toString();
    });
    child.stdout.pipe(log, { end: false });
    child.stderr.pipe(log, { end: false });
    const flushed = finished(log);
    let failure: Error | undefined;
    child.once('error', (error) => {
      failure = error;
    });
    const code = await new Promise<number | null>((resolve) => {
      child.once('close', resolve);
    });
    log.end();
    await flushed;
    signal?.throwIfAborted();
    if (failure) throw failure;
    assert.equal(code, 0, `Docker ${label}; log=${logDir}`);
    return output.trim();
  }
  function compose(args: string[], label: string, signal?: AbortSignal): Promise<string> {
    return docker(['compose', '-p', project, '-f', 'docker-compose.yml', ...args], label, signal);
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
    const node = new NodeProcess(config, role, command, args, logDir, all.length, serverConsole);
    all.push(node);
    nodes.set(config.id, node);
    return node;
  }

  const control: NodeControl = {
    async drain(id) {
      const node = nodes.get(id);
      assert(node, `Unknown host ${id}`);
      await node.drain();
    },
    async replace(id) {
      const node = nodes.get(id);
      assert(node, `Unknown host ${id}`);
      await node.stop();
      const replacement = await start(node.config, node.role);
      await replacement.ready();
    }
  };

  async function startAll(signal?: AbortSignal): Promise<Omit<ServerState, 'controlEndpoint'>> {
    const structure = JSON.parse(readFileSync(LocalHost.structureFile, 'utf8')) as {
      nodes: { role: string; config: string | null }[];
    };
    const servers = structure.nodes.filter(
      (node): node is { role: NodeRole; config: string } =>
        (node.role === 'session' || node.role === 'table') && node.config !== null
    );
    const configs = servers.map((node) => ({ role: node.role, config: readConfig(node.config) }));
    for (const { config } of configs) serverConsole?.addServer(config.id, config.title);
    if (!dockerProject) await compose(['up', '-d', '--wait'], 'up', signal);
    const redis = dockerProject
      ? LocalHost.dockerRedis
      : await compose(['port', 'redis', LocalHost.redisPort], 'port', signal);
    serverConsole?.host(`Redis started — listen ${redis}`);
    for (const { role, config } of configs) {
      signal?.throwIfAborted();
      config.tableCapacity = tableCapacity;
      config.redisEndpoint = redis;
      config.keyPrefix = keyPrefix;
      config.meshEndpoint = LocalHost.meshEndpoint;
      if (role === 'session') config.streamEndpoint = LocalHost.streamEndpoint;
      if (dockerProject) config.logDir = LogFiles.container;
      await start(config, role);
    }
    const ready = await Promise.all([...nodes.values()].map((node) => node.ready(signal)));
    const session = [...nodes.values()].find((node) => node.role === 'session');
    assert(session);
    const sessionEndpoint = ready.find((node) => node.id === session.config.id)!.endpoint;
    return { sessionEndpoint, runId, keyPrefix, nodes: ready.map((node) => node.id) };
  }

  async function stopAll(): Promise<boolean> {
    let failed = false;
    for (const node of all.reverse()) {
      try {
        await node.stop();
      } catch (error) {
        failed = true;
        console.error('Node cleanup failed', error);
      }
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
      if (!dockerProject) serverConsole?.host('Redis stopped');
    } catch (error) {
      failed = true;
      console.error('Redis cleanup failed', error);
    }
    return !failed;
  }

  // Host lines for a standalone server's console; managed e2e runs print nothing here.
  const announce = (message: string): void => serverConsole?.host(message);
  return { logDir, control, startAll, stopAll, announce };
}

async function main(): Promise<void> {
  const stopping = new AbortController();
  const stop = (): void => stopping.abort();
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  const options: ConsoleOptions = { trace: process.argv.includes(LocalHost.traceFlag) };
  checkStructure();
  await assertNoRunningServer();
  const host = createServerHost(Scenarios.length, options);
  try {
    const state = await host.startAll(stopping.signal);
    if (stopping.signal.aborted) return;
    const unpublish = await publishServer(state, host.control);
    try {
      host.announce(`${LocalHost.ready} ${state.sessionEndpoint} (game clients connect here)`);
      host.announce(
        `Logs: ${LogFiles.directory}/<server>.log` +
          (options.trace ? '' : ` (message flow: add ${LocalHost.traceFlag} to show it here)`)
      );
      if (!stopping.signal.aborted) {
        await new Promise<void>((resolve) => {
          stopping.signal.addEventListener('abort', () => resolve(), { once: true });
        });
      }
    } finally {
      await unpublish();
    }
  } catch (error) {
    if (!stopping.signal.aborted || error !== stopping.signal.reason) throw error;
  } finally {
    host.announce('Stopping servers');
    if (!(await host.stopAll())) process.exitCode = 1;
    process.off('SIGINT', stop);
    process.off('SIGTERM', stop);
  }
}

if (require.main === module) {
  main().catch((error) => {
    console.error(error instanceof ServerAlreadyRunningError ? error.message : error);
    process.exitCode = 1;
  });
}
