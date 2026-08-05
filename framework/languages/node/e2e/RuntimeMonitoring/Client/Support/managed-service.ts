import { spawn, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import type { ClientOptions } from './client-options';
import { postJson } from '../../../http-client';

export function startServiceB(options: ClientOptions, logName: string): ManagedProcess {
  return startService(options, options.filteredServiceMain, options.serviceBConfig, logName);
}

export function startReplacementService(options: ClientOptions, logName: string): ManagedProcess {
  return startService(options, options.filteredServiceMain, options.replacementServiceConfig, logName);
}

function startService(options: ClientOptions, main: string, config: string, logName: string): ManagedProcess {
  const stdout = fs.openSync(`${options.logDir}/${logName}.stdout.log`, 'w');
  const stderr = fs.openSync(`${options.logDir}/${logName}.stderr.log`, 'w');
  const child = spawn(process.execPath, [main, '--config', config], {
    stdio: ['ignore', stdout, stderr]
  });
  return new ManagedProcess(child);
}

export class ManagedProcess {
  constructor(private readonly child: ChildProcess) {}

  async stop(): Promise<void> {
    try {
      await this.wait(5000);
      return;
    } catch {
    }
    this.child.kill('SIGTERM');
    try {
      await this.wait(5000);
      return;
    } catch {
    }
    this.child.kill('SIGKILL');
    await this.wait(5000);
  }

  async wait(timeoutMs = 30000): Promise<void> {
    if (this.child.exitCode !== null || this.child.signalCode !== null) return;
    await Promise.race([
      new Promise<void>((resolve) => this.child.once('exit', () => resolve())),
      new Promise<void>((_resolve, reject) => setTimeout(() => reject(new Error('Service process did not exit.')), timeoutMs))
    ]);
  }
}

export async function postBestEffort(baseUrl: string, path: string): Promise<void> {
  try {
    await postJson<object>(baseUrl, path, {});
  } catch {
  }
}

export async function waitForPortState(baseUrl: string, shouldBeOpen: boolean, failureMessage: string): Promise<void> {
  const url = new URL(baseUrl);
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (await canConnect(url.hostname, Number(url.port)) === shouldBeOpen) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(failureMessage);
}

async function canConnect(host: string, port: number): Promise<boolean> {
  return await new Promise<boolean>((resolve) => {
    const socket = net.createConnection({ host, port });
    socket.setTimeout(200);
    socket.once('connect', () => { socket.destroy(); resolve(true); });
    socket.once('timeout', () => { socket.destroy(); resolve(false); });
    socket.once('error', () => resolve(false));
  });
}
