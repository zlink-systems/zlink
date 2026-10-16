import assert from 'node:assert/strict';
import { spawn, type ChildProcess } from 'node:child_process';
import { createWriteStream, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { createInterface } from 'node:readline';
import { EventEmitter } from 'node:events';
import { finished } from 'node:stream/promises';
import { checkStructure } from './structure';
import {
  Game,
  zoneId,
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
    command: string,
    args: string[],
    logDir: string,
    sequence: number
  ) {
    const out = createWriteStream(`${logDir}/${config.id}-${sequence}.out.log`),
      err = createWriteStream(`${logDir}/${config.id}-${sequence}.err.log`);
    this.child = spawn(command, args, {
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe']
    });
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
    this.child.stdin!.write(Control.vacateZone + '\n');
    await this.wait(Control.zoneVacated, Limits.messageMs);
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
  const runId = randomUUID();
  const logDir = `.logs/run-${runId}`;
  mkdirSync(logDir, { recursive: true });
  const project = `my-game-${runId}`;
  const dockerProject = process.env.MY_GAME_DOCKER_PROJECT;
  const containers: string[] = [];
  const nodes = new Map<string, NodeProcess>();
  const all: NodeProcess[] = [];
  let passed = 0;
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
    const node = new NodeProcess(config, role, command, args, logDir, all.length);
    all.push(node);
    nodes.set(config.id, node);
    return node;
  }
  try {
    if (!dockerProject) await compose(['up', '-d', '--wait'], 'up');
    const redis = dockerProject ? 'redis:6379' : await compose(['port', 'redis', '6379'], 'port');
    const structure = JSON.parse(readFileSync('zlink.structure.json', 'utf8')) as {
      nodes: { role: string; config: string | null }[];
    };
    const applicationNodes = structure.nodes.filter((n) => n.role in NodeEntrypoints);
    // Bootstrap fixed Zones before peers can receive placement requests.
    const startupOrder = ['zone', 'instance', 'session'];
    applicationNodes.sort((a, b) => startupOrder.indexOf(a.role) - startupOrder.indexOf(b.role));
    for (const node of applicationNodes) {
      if (node.role !== 'session' && node.role !== 'zone' && node.role !== 'instance') continue;
      assert(node.config);
      const cfg = JSON.parse(readFileSync(node.config, 'utf8')) as NodeConfig;
      if (node.role === 'zone' && !cfg.zoneIds!.includes(zoneId(Control.replacementX)))
        cfg.zoneCapacity = Game.zoneCount - 1;
      cfg.redisEndpoint = redis;
      cfg.keyPrefix = `my-game:${runId}:`;
      cfg.meshEndpoint = 'tcp://127.0.0.1:0';
      if (node.role === 'session') cfg.streamEndpoint = 'ws://127.0.0.1:0';
      const started = await start(cfg, node.role);
      await started.ready();
    }
    const ready = await Promise.all([...nodes.values()].map((n) => n.ready()));
    const session = [...nodes.values()].find((n) => n.role === 'session');
    assert(session);
    const endpoint = ready.find((r) => r.id === session.config.id)!.endpoint;
    const { scenario } = await import('./client.mjs');
    const control = {
      async drain(id: string): Promise<void> {
        const node = nodes.get(id);
        assert(node, `Unknown host ${id}`);
        await node.drain();
      },
      async replace(id: string): Promise<void> {
        const node = nodes.get(id);
        assert(node);
        await node.stop();
        const replacement = await start({ ...node.config, bootstrap: false }, node.role);
        await replacement.ready();
      }
    };
    for (const name of selected) {
      try {
        await scenario(name, endpoint, runId, logDir, control);
        passed++;
        console.log(`✓ ${name}`);
      } catch (error) {
        failed = true;
        console.error(`✗ ${name}`, error);
      }
    }
  } catch (error) {
    failed = true;
    console.error('Startup failed', error);
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
  }
  console.log(`Scenarios: ${passed}/${selected.length} passed`);
  console.log(`Logs: ${logDir}`);
  process.exitCode = failed ? 1 : 0;
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
