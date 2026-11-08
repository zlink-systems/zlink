#!/usr/bin/env node
// Install CLI signal handlers before loading the gRPC dependency graph.
function watchTermination() {
  const stop = new AbortController();
  const onSignal = () => stop.abort();
  process.on('SIGINT', onSignal);
  process.on('SIGTERM', onSignal);
  return {
    stop,
    close: () => {
      process.removeListener('SIGINT', onSignal);
      process.removeListener('SIGTERM', onSignal);
    }
  };
}

const entryTermination = require.main === module ? watchTermination() : undefined;

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { EtcdClient, EtcdCallError } from './internal/client';
import { ClockEngine, EtcdAdminStore, MissingClockError, systemTime } from './internal/admin-engine';
import { EtcdOptions, validateOptions } from './internal/options';

const CLEANUP_INTERVAL_MS = 60_000;
const ERROR_WAIT_MS = 1000;

interface FileOptions {
  endpoints: string[];
  namespace: string;
  operationTimeoutMs?: number;
  tls?: { trustedCaPemFile?: string; clientIdentity?: { certificatePemFile: string; keyPemFile: string } | null } | null;
  credentials?: { user: string; passwordFile: string };
}

function readConfig(file: string): EtcdOptions {
  const config = JSON.parse(readFileSync(file, 'utf8')) as FileOptions;
  const read = (name: string) => readFileSync(resolve(dirname(file), name), 'utf8');
  const tls = config.tls;
  if (tls === null || (tls !== undefined && (typeof tls !== 'object' || Array.isArray(tls)))) throw new TypeError('Invalid TLS configuration');
  const identity = tls?.clientIdentity;
  if (identity === null) throw new TypeError('Invalid client identity');
  const options: EtcdOptions = {
    endpoints: config.endpoints,
    namespace: config.namespace,
    operationTimeoutMs: config.operationTimeoutMs,
    tls: tls === undefined ? undefined : {
      trustedCaPem: tls.trustedCaPemFile === undefined ? undefined : read(tls.trustedCaPemFile),
      clientIdentity: identity === undefined ? undefined : {
        certificatePem: read(identity.certificatePemFile), keyPem: read(identity.keyPemFile)
      }
    },
    credentials: config.credentials === undefined ? undefined : {
      user: config.credentials.user, password: read(config.credentials.passwordFile)
    }
  };
  validateOptions(options);
  return options;
}

function report(error: unknown): void {
  if (error instanceof MissingClockError) process.stderr.write('Store clock key is missing\n');
  else if (error instanceof EtcdCallError) process.stderr.write(`etcd ${error.kind} (gRPC ${error.code})\n`);
  else process.stderr.write('Store maintenance failed\n');
}

async function run(store: EtcdAdminStore, stop: AbortController): Promise<number> {
  let result = 0;
  const stopped = () => stop.signal.aborted;
  const engine = new ClockEngine(store);
  const loop = async (action: () => Promise<void>, interval: number) => {
    while (!stop.signal.aborted) {
      try { await action(); }
      catch (error) {
        if (stopped()) return;
        report(error);
        if (error instanceof MissingClockError) { result = 3; stop.abort(); return; }
        await systemTime.sleep(ERROR_WAIT_MS, stop.signal);
      }
      if (interval > 0) await systemTime.sleep(interval, stop.signal);
    }
  };
  await Promise.all([
    loop(() => engine.step(stop.signal), 0),
    loop(() => store.cleanup(stop.signal), CLEANUP_INTERVAL_MS)
  ]);
  return result;
}

export async function main(args = process.argv.slice(2)): Promise<number> {
  const termination = entryTermination ?? watchTermination();
  try {
    let options: EtcdOptions;
    const command = args[0];
    try {
      if (args.length !== 3 || !['init', 'run', 'cleanup'].includes(command) || args[1] !== '--config') throw new TypeError('Invalid arguments');
      options = readConfig(resolve(args[2]));
    } catch {
      process.stderr.write('Invalid command or configuration\n');
      return 1;
    }
    if (command === 'run' && termination.stop.signal.aborted) return 0;
    let client: EtcdClient | undefined;
    try {
      client = new EtcdClient(options);
      const store = new EtcdAdminStore(client);
      if (command === 'init') return await store.init() ? 0 : 2;
      if (command === 'run') return await run(store, termination.stop);
      await store.cleanup();
      return 0;
    } catch (error) {
      report(error);
      return error instanceof MissingClockError ? 3 : 4;
    } finally { client?.close(); }
  } finally { termination.close(); }
}

if (require.main === module) void main().then(code => { process.exitCode = code; });
