import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import type { ChildProcess } from 'node:child_process';
import { getStatus, postStatus } from '../../../http-client';

export interface ProviderStartOptions {
  readonly providerMain: string;
  readonly logDir: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly name: string;
  readonly rid: string;
  readonly httpUrl: string;
  readonly channelEndpoint: string;
  readonly evidenceFileName: string;
}

export function startProvider(options: ProviderStartOptions): ManagedProcess {
  fs.mkdirSync(options.logDir, { recursive: true });
  const config = path.join(options.logDir, `${options.name}.config.json`);
  fs.writeFileSync(config, `${JSON.stringify({ e2e: {
    rid: options.rid,
    httpUrl: options.httpUrl,
    redisEndpoint: options.redisEndpoint,
    redisKeyPrefix: options.redisKeyPrefix,
    channelEndpoint: options.channelEndpoint,
    evidenceFile: path.join(options.logDir, options.evidenceFileName),
    logDir: options.logDir
  } }, null, 2)}\n`, { mode: 0o600 });
  const child = spawn(process.execPath, [
    options.providerMain,
    '--config', config
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout?.pipe(fs.createWriteStream(path.join(options.logDir, `${options.name}.stdout.log`)));
  child.stderr?.pipe(fs.createWriteStream(path.join(options.logDir, `${options.name}.stderr.log`)));
  return new ManagedProcess(child, options.httpUrl);
}

export class ManagedProcess {
  constructor(private readonly child: ChildProcess, private readonly healthUrl: string) {}

  async waitReady(): Promise<void> {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      if (this.child.exitCode !== null) {
        throw new Error(`Provider exited before readiness: ${this.child.exitCode}`);
      }
      try {
        const status = await getStatus(`${this.healthUrl}/health`);
        if (status >= 200 && status < 300) {
          return;
        }
      } catch {
      }
      await delay(250);
    }
    throw new Error(`Provider did not become ready: ${this.healthUrl}`);
  }

  async stop(): Promise<void> {
    if (this.child.exitCode !== null) {
      return;
    }
    const exited = new Promise<void>((resolve) => this.child.once('exit', () => resolve()));
    try {
      await postStatus(`${this.healthUrl}/shutdown`);
    } catch {
      this.child.kill('SIGTERM');
    }
    const killer = setTimeout(() => {
      if (this.child.exitCode === null) {
        this.child.kill('SIGKILL');
      }
    }, 5000);
    await exited.finally(() => clearTimeout(killer));
  }

  async waitExited(timeoutMs = 10000): Promise<void> {
    if (this.child.exitCode !== null) {
      return;
    }
    const exited = new Promise<void>((resolve) => this.child.once('exit', () => resolve()));
    const timeout = new Promise<never>((_, reject) => setTimeout(() => reject(new Error('Provider did not exit.')), timeoutMs));
    await Promise.race([exited, timeout]);
  }
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
