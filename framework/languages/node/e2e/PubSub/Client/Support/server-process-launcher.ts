import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import type { ChildProcess } from 'node:child_process';
import type { ClientOptions } from './client-options';
import { getStatus, postStatus } from '../../../http-client';

export class ServerProcessLauncher {
  constructor(private readonly options: ClientOptions) {}

  startSubscriber(
    name: string,
    httpUrl: string,
    evidenceFile: string,
    publisherEndpoint?: string,
    subscriberMode?: 'mixed'
  ): DynamicProcess {
    const config = this.writeConfig(name, {
      rid: name,
      httpUrl,
      evidenceFile: path.join(this.options.logDir, evidenceFile),
      logDir: this.options.logDir,
      handlerDelayMs: 0,
      publisherEndpoint,
      subscriberMode,
      redisEndpoint: this.options.redisEndpoint,
      redisKeyPrefix: this.options.redisKeyPrefix
    });
    return this.start(
      name,
      this.options.subscriberMain,
      ['--config', config],
      httpUrl
    );
  }

  startSubscriberWithRedis(
    name: string,
    httpUrl: string,
    evidenceFile: string,
    redisEndpoint: string,
    redisKeyPrefix: string
  ): DynamicProcess {
    const config = this.writeConfig(name, {
      rid: name,
      httpUrl,
      evidenceFile: path.join(this.options.logDir, evidenceFile),
      logDir: this.options.logDir,
      handlerDelayMs: 0,
      redisEndpoint,
      redisKeyPrefix
    });
    return this.start(name, this.options.subscriberMain, ['--config', config], httpUrl);
  }

  startPublisher(): DynamicProcess {
    return this.startPublisherNamed(
      'pub-restart',
      this.options.publisherUrl,
      this.options.publisherEndpoint,
      'pub-a'
    );
  }

  startPublisherNamed(
    name: string,
    httpUrl: string,
    publisherEndpoint: string,
    rid = name,
    publisherAdvertiseHost?: string
  ): DynamicProcess {
    const config = this.writeConfig(name, {
      rid,
      httpUrl,
      publisherEndpoint,
      publisherAdvertiseHost,
      redisEndpoint: this.options.redisEndpoint,
      redisKeyPrefix: this.options.redisKeyPrefix,
      evidenceFile: path.join(this.options.logDir, `${name}.evidence.log`),
      logDir: this.options.logDir
    });
    return this.start(name, this.options.publisherMain, ['--config', config], httpUrl);
  }

  startPublisherIdentityValidation(
    name: string,
    httpUrl: string,
    publisherEndpoint: string,
    publisherIdentityMode: 'missing' | 'both',
    redisEndpoint: string,
    redisKeyPrefix: string
  ): DynamicProcess {
    const config = this.writeConfig(name, {
      rid: name,
      httpUrl,
      publisherEndpoint,
      publisherIdentityMode,
      redisEndpoint,
      redisKeyPrefix,
      evidenceFile: path.join(this.options.logDir, `${name}.evidence.log`),
      logDir: this.options.logDir
    });
    return this.start(
      name,
      this.options.publisherMain,
      ['--config', config],
      httpUrl
    );
  }

  private start(name: string, mainPath: string, args: readonly string[], httpUrl: string): DynamicProcess {
    fs.mkdirSync(this.options.logDir, { recursive: true });
    const child = spawn(process.execPath, [mainPath, ...args], {
      stdio: ['ignore', 'pipe', 'pipe']
    });
    child.stdout?.pipe(fs.createWriteStream(path.join(this.options.logDir, `${name}.stdout.log`)));
    child.stderr?.pipe(fs.createWriteStream(path.join(this.options.logDir, `${name}.stderr.log`)));
    return new DynamicProcess(child, httpUrl);
  }

  private writeConfig(name: string, e2e: Record<string, unknown>): string {
    fs.mkdirSync(this.options.logDir, { recursive: true });
    const config = path.join(this.options.logDir, `${name}.config.json`);
    fs.writeFileSync(config, `${JSON.stringify({ e2e }, null, 2)}\n`, { mode: 0o600 });
    return config;
  }
}

export class DynamicProcess {
  constructor(
    private readonly child: ChildProcess,
    readonly httpUrl: string
  ) {}

  get hasExited(): boolean {
    return this.child.exitCode !== null || this.child.signalCode !== null;
  }

  get exitCode(): number | null {
    return this.child.exitCode;
  }

  async waitReady(): Promise<void> {
    for (let i = 0; i < 120; i += 1) {
      if (this.hasExited) {
        throw new Error(`Process exited before readiness: ${this.child.exitCode ?? this.child.signalCode}`);
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

  async waitForExit(timeoutMs = 5000): Promise<number | null> {
    if (this.hasExited) return this.exitCode;
    return new Promise<number | null>((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new Error(`Process did not exit within ${timeoutMs}ms: ${this.httpUrl}`));
      }, timeoutMs);
      this.child.once('exit', (code) => {
        clearTimeout(timer);
        resolve(code);
      });
    });
  }

  async stop(): Promise<void> {
    if (this.hasExited) {
      return;
    }
    const exited = new Promise<void>((resolve) => {
      this.child.once('exit', () => resolve());
    });
    try {
      await postStatus(`${this.httpUrl}/shutdown`);
    } catch {
      this.child.kill('SIGTERM');
    }
    if (this.hasExited) {
      return;
    }
    const killer = setTimeout(() => {
      if (!this.hasExited) {
        this.child.kill('SIGKILL');
      }
    }, 5000);
    await exited.finally(() => clearTimeout(killer));
  }

  async kill(): Promise<void> {
    if (this.hasExited) {
      return;
    }
    const exited = new Promise<void>((resolve) => {
      this.child.once('exit', () => resolve());
    });
    this.child.kill('SIGKILL');
    await exited;
  }
}
