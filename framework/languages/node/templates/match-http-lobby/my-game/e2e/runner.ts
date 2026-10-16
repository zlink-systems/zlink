import assert from 'node:assert/strict';
import { spawn, type ChildProcess } from 'node:child_process';
import { createWriteStream, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { createInterface } from 'node:readline';
import { EventEmitter } from 'node:events';
import { finished } from 'node:stream/promises';
import { checkStructure } from './structure';
import {
  Limits,
  Scenarios,
  Control,
  NodeEntrypoints,
  type NodeRole
} from '../server/shared/constants';
import type { NodeConfig } from '../server/shared/config';
class NodeProcess {
  readonly child: ChildProcess;
  private readonly lines: string[] = [];
  private readonly changed = new EventEmitter();
  private exited = false;
  readonly exit: Promise<Error | undefined>;
  constructor(
    readonly config: NodeConfig,
    readonly role: NodeRole,
    readonly path: string,
    logDir: string,
    sequence: number
  ) {
    writeFileSync(path, JSON.stringify(config, null, 2));
    const out = createWriteStream(`${logDir}/${config.id}-${sequence}.out.log`),
      err = createWriteStream(`${logDir}/${config.id}-${sequence}.err.log`);
    this.child = spawn(
      process.execPath,
      [`dist/${NodeEntrypoints[role].replace(/\.ts$/, '.js')}`, path],
      {
        windowsHide: true,
        stdio: ['pipe', 'pipe', 'pipe']
      }
    );
    this.child.stdout!.pipe(out);
    this.child.stderr!.pipe(err);
    const reader = createInterface({ input: this.child.stdout! });
    reader.on('line', (line) => {
      this.lines.push(line);
      this.changed.emit('change');
    });
    this.exit = new Promise<Error | undefined>((resolve) => {
      this.child.once('error', resolve);
      this.child.once('close', (code, signal) => {
        this.exited = true;
        this.changed.emit('change');
        reader.close();
        Promise.all([finished(out), finished(err)]).then(
          () =>
            code === 0
              ? resolve(undefined)
              : resolve(new Error(`${config.id} exit=${code} signal=${signal}; logs=${logDir}`)),
          (error: Error) => resolve(error)
        );
      });
    });
  }
  async wait(prefix: string, timeoutMs: number): Promise<string> {
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
        reject(new Error(`${this.config.id} timeout waiting for ${prefix}`));
      }, timeoutMs);
      this.changed.on('change', check);
      check();
    });
  }
  async ready(): Promise<{ id: string; endpoint: string }> {
    return JSON.parse(
      (await this.wait(Control.ready, Limits.startupMs)).slice(Control.ready.length)
    ) as {
      id: string;
      endpoint: string;
    };
  }
  async drain(): Promise<void> {
    this.child.stdin!.write(Control.drain + '\n');
    await this.wait(Control.requested, Limits.messageMs);
    await this.wait(Control.relocated, Limits.relocationMs);
    await this.wait(Control.safe, Limits.relocationMs);
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
async function main(): Promise<void> {
  const args = process.argv.slice(2);
  if (args[0] === '--check-structure') {
    assert(args.length <= 2);
    checkStructure(args[1]);
    return;
  }
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
  let passed = 0;
  for (const name of selected) if (await runScenario(name)) passed++;
  console.log(`Scenarios: ${passed}/${selected.length} passed`);
  process.exitCode = passed === selected.length ? 0 : 1;
}
async function runScenario(name: string): Promise<boolean> {
  const runId = randomUUID();
  const logDir = `.logs/run-${runId}`;
  mkdirSync(logDir, { recursive: true });
  const project = `my-game-${runId}`;
  const nodes = new Map<string, NodeProcess>();
  const all: NodeProcess[] = [];
  let failed = false;
  async function docker(args: string[], label: string): Promise<string> {
    const log = createWriteStream(`${logDir}/docker-${label}.log`);
    const child = spawn('docker', ['compose', '-p', project, '-f', 'docker-compose.yml', ...args], {
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
  async function start(config: NodeConfig, role: NodeRole): Promise<NodeProcess> {
    const node = new NodeProcess(config, role, `${logDir}/${config.id}.json`, logDir, all.length);
    all.push(node);
    nodes.set(config.id, node);
    return node;
  }
  try {
    await docker(['up', '-d', '--wait'], 'up');
    const redis = await docker(['port', 'redis', '6379'], 'port');
    const structure = JSON.parse(readFileSync('zlink.structure.json', 'utf8')) as {
      nodes: { role: string; config: string | null }[];
    };
    const applicationNodes = structure.nodes
      .filter((n) => n.role === 'match' || n.role === 'session' || n.role === 'http-lobby')
      .map((node) => {
        assert(node.config);
        return {
          role: node.role as NodeRole,
          config: JSON.parse(readFileSync(node.config, 'utf8')) as NodeConfig
        };
      });
    const roleOrder = { match: 0, session: 1, 'http-lobby': 2 } as const;
    applicationNodes.sort((a, b) => roleOrder[a.role] - roleOrder[b.role]);
    for (const node of applicationNodes) {
      const cfg = node.config;
      cfg.redisEndpoint = redis;
      cfg.keyPrefix = `my-game:${runId}:`;
      cfg.meshEndpoint = 'tcp://127.0.0.1:0';
      if (node.role === 'match') cfg.roomCapacity = 1;
      if (node.role === 'session') cfg.streamEndpoint = 'ws://127.0.0.1:0';
      if (node.role === 'http-lobby') {
        cfg.httpEndpoint = 'http://127.0.0.1:0';
        const session = [...nodes.values()].find((n) => n.role === 'session');
        assert(session);
        cfg.sessionEndpoint = (await session.ready()).endpoint;
      }
      const started = await start(cfg, node.role);
      await started.ready();
    }
    const ready = await Promise.all([...nodes.values()].map((n) => n.ready()));
    const session = [...nodes.values()].find((n) => n.role === 'session');
    assert(session);
    const endpoint = ready.find((r) => r.id === session.config.id)!.endpoint;
    const { scenario } = await import('./client.mjs');
    const control = {
      isLiveMatch(id: string): boolean {
        const node = nodes.get(id);
        return (
          node?.role === 'match' && node.child.exitCode === null && node.child.signalCode === null
        );
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
        const replacement = await start({ ...node.config }, node.role);
        await replacement.ready();
      }
    };
    const lobby = [...nodes.values()].find((n) => n.role === 'http-lobby');
    assert(lobby);
    await scenario(name, endpoint, (await lobby.ready()).endpoint, runId, logDir, control);
  } catch (error) {
    failed = true;
    console.error(`Scenario ${name} failed`, error);
  } finally {
    for (const node of all.reverse())
      try {
        await node.stop();
      } catch (error) {
        failed = true;
        console.error('Node cleanup failed', error);
      }
    try {
      await docker(['down', '--volumes', '--remove-orphans'], 'down');
    } catch (error) {
      failed = true;
      console.error('Redis cleanup failed', error);
    }
  }
  console.log(`${failed ? '✗' : '✓'} ${name}`);
  console.log(`Logs: ${logDir}`);
  return !failed;
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
