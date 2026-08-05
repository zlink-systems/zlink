import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { spawn } from 'node:child_process';
import type { ChildProcess } from 'node:child_process';
import type { ClientOptions } from './client-options';
import { getJson, getStatus, postJsonWithin, postStatus } from '../../../http-client';

interface RelocationResult {
  readonly outcome: number;
  readonly reason: number;
}

export interface DynamicProvider {
  readonly process: DynamicProcess;
  readonly httpUrl: string;
  readonly channelEndpoint: string;
  readonly routeEndpoint: string;
}

export interface DynamicConsumer {
  readonly process: DynamicProcess;
  readonly httpUrl: string;
}

export class DynamicClusterLauncher {
  private readonly processes: DynamicProcess[] = [];
  private topologyConsumer?: DynamicConsumer;
  private constructor(
    private readonly providerMain: string,
    private readonly consumerMain: string,
    private readonly logDir: string,
    private readonly redisEndpoint: string,
    private readonly redisKeyPrefix: string
  ) {}

  static async start(options: ClientOptions, scenarioName: string): Promise<DynamicClusterLauncher> {
    const launcher = new DynamicClusterLauncher(
      options.providerMain,
      options.consumerMain,
      options.logDir,
      options.redisEndpoint,
      `${options.redisKeyPrefix}:${scenarioName}`
    );
    return launcher;
  }

  async startProvider(
    name: string,
    rid: string,
    weight = 100,
    routePeers: readonly string[] = []
  ): Promise<DynamicProvider> {
    const httpUrl = await pickHttpUrl();
    const channelEndpoint = await pickEndpoint();
    const routeEndpoint = await pickEndpoint();
    let process: DynamicProcess | undefined;
    try {
      process = this.startServer(
        name,
        this.providerMain,
        {
          rid, httpUrl, redisEndpoint: this.redisEndpoint, redisKeyPrefix: this.redisKeyPrefix,
          channelEndpoint, routeEndpoint, routePeers, weight,
          evidenceFile: path.join(this.logDir, `${name}.evidence.log`), logDir: this.logDir
        },
        httpUrl,
        channelEndpoint
      );
      await process.waitReady();
      return { process, httpUrl, channelEndpoint, routeEndpoint };
    } catch (error) {
      if (process !== undefined) {
        await process.stop();
        const index = this.processes.indexOf(process);
        if (index >= 0) {
          this.processes.splice(index, 1);
        }
      }
      throw error;
    }
  }

  async startConsumer(name: string): Promise<DynamicConsumer> {
    const httpUrl = await pickHttpUrl();
    let process: DynamicProcess | undefined;
    try {
      process = this.startServer(
        name,
        this.consumerMain,
        {
          httpUrl,
          redisEndpoint: this.redisEndpoint,
          redisKeyPrefix: this.redisKeyPrefix,
          traceLabel: name,
          logDir: this.logDir
        },
        httpUrl
      );
      await process.waitReady();
      const consumer = { process, httpUrl };
      this.topologyConsumer ??= consumer;
      return consumer;
    } catch (error) {
      if (process !== undefined) {
        await process.stop();
        const index = this.processes.indexOf(process);
        if (index >= 0) this.processes.splice(index, 1);
      }
      throw error;
    }
  }

  async waitForSingleProvider(rid: string, endpoint: string): Promise<void> {
    await this.waitForProviders([endpoint], `Provider '${rid}'`);
  }

  async waitForProviders(endpoints: readonly string[], description = 'Providers'): Promise<void> {
    const topology = this.topologyConsumer;
    if (topology === undefined) throw new Error('Location consumer is not running.');
    const expected = [...endpoints].sort();
    for (let i = 0; i < 120; i += 1) {
      const rows = await getJson<Array<{ readonly endpoint?: string }>>(`${topology.httpUrl}/location/topology`);
      const actual = rows.flatMap((row) => row.endpoint === undefined ? [] : [row.endpoint]).sort();
      if (actual.length === expected.length && actual.every((value, index) => value === expected[index])) {
        const status = await getJson<DynamicRouteStatus>(`${topology.httpUrl}/route/status`);
        const profile = status.channels.find((channel) => channel.channelName === 'profile');
        const readyPeerCount = status.peers.filter((peer) => peer.state === 1).length;
        if (profile?.isReady === true
            && profile.readyTargetCount === expected.length
            && readyPeerCount >= expected.length) {
          return;
        }
      }
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
    throw new Error(`${description} did not converge to '${expected.join(',')}'.`);
  }

  async stop(provider: DynamicProvider): Promise<void> {
    await provider.process.stop();
    const index = this.processes.indexOf(provider.process);
    if (index >= 0) {
      this.processes.splice(index, 1);
    }
  }

  async drain(provider: DynamicProvider): Promise<RelocationResult> {
    const result = await postJsonWithin<RelocationResult>(
      provider.httpUrl,
      '/drain',
      {},
      35_000
    );
    await provider.process.stop();
    this.forget(provider.process);
    return result;
  }

  async crash(provider: DynamicProvider): Promise<void> {
    await provider.process.crash();
    this.forget(provider.process);
  }

  private forget(process: DynamicProcess): void {
    const index = this.processes.indexOf(process);
    if (index >= 0) this.processes.splice(index, 1);
  }

  async close(): Promise<void> {
    for (let i = this.processes.length - 1; i >= 0; i -= 1) {
      await this.processes[i].stop();
    }
    this.processes.length = 0;
  }

  private startServer(name: string, mainPath: string, e2e: Record<string, unknown>, httpUrl: string, channelEndpoint?: string): DynamicProcess {
    const config = path.join(this.logDir, `${name}.config.json`);
    fs.mkdirSync(this.logDir, { recursive: true });
    fs.writeFileSync(config, `${JSON.stringify({ e2e }, null, 2)}\n`, { mode: 0o600 });
    const child = spawn(process.execPath, [mainPath, '--config', config], { stdio: ['ignore', 'pipe', 'pipe'] });
    fs.mkdirSync(this.logDir, { recursive: true });
    child.stdout?.pipe(fs.createWriteStream(path.join(this.logDir, `${name}.stdout.log`)));
    child.stderr?.pipe(fs.createWriteStream(path.join(this.logDir, `${name}.stderr.log`)));
    const dynamicProcess = new DynamicProcess(child, httpUrl, channelEndpoint);
    this.processes.push(dynamicProcess);
    return dynamicProcess;
  }
}

interface DynamicRouteStatus {
  readonly peers: readonly { readonly state: number }[];
  readonly channels: readonly {
    readonly channelName: string;
    readonly isReady: boolean;
    readonly readyTargetCount: number;
  }[];
}

export class DynamicProcess {
  constructor(
    private readonly process: ChildProcess,
    readonly httpUrl: string,
    readonly channelEndpoint?: string
  ) {}

  async waitReady(): Promise<void> {
    for (let i = 0; i < 120; i += 1) {
      if (this.process.exitCode !== null) {
        throw new Error(`Process exited before readiness: ${this.process.exitCode}`);
      }
      try {
        const status = await getStatus(`${this.httpUrl}/health`);
        if (status >= 200 && status < 300) {
          return;
        }
      } catch {
      }
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
    throw new Error(`Process did not become ready: ${this.httpUrl}`);
  }

  async stop(): Promise<void> {
    if (this.process.exitCode !== null) {
      return;
    }
    const exited = new Promise<void>((resolve) => {
      this.process.once('exit', () => resolve());
    });
    if (this.process.exitCode !== null) {
      return;
    }
    const killer = setTimeout(() => {
      if (this.process.exitCode === null) {
        this.process.kill('SIGKILL');
      }
    }, 5000);
    try {
      await Promise.race([
        postStatus(`${this.httpUrl}/shutdown`, 1000),
        new Promise((_, reject) => setTimeout(() => reject(new Error('shutdown request timed out')), 1000))
      ]);
    } catch {
      if (this.process.exitCode === null) {
        this.process.kill('SIGTERM');
      }
    }
    await exited.finally(() => clearTimeout(killer));
  }

  async crash(): Promise<void> {
    if (this.process.exitCode !== null) return;
    const exited = this.exitPromise();
    this.process.kill('SIGKILL');
    await exited;
  }

  async waitExited(): Promise<void> {
    if (this.process.exitCode !== null) return;
    await this.exitPromise();
  }

  private exitPromise(): Promise<void> {
    return new Promise<void>((resolve) => this.process.once('exit', () => resolve()));
  }
}

async function pickEndpoint(): Promise<string> {
  return `tcp://127.0.0.1:${await pickPort()}`;
}

async function pickHttpUrl(): Promise<string> {
  return `http://127.0.0.1:${await pickPort()}`;
}

function pickPort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      const port = typeof address === 'object' && address !== null ? address.port : 0;
      server.close(() => resolve(port));
    });
  });
}
