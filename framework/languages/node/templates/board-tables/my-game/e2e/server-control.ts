import assert from 'node:assert/strict';
import { linkSync, mkdirSync, readFileSync, unlinkSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { Control, Limits, LocalHost, LogFiles } from '../server/shared/constants';

export interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
}

export interface ServerState {
  controlEndpoint: string;
  sessionEndpoint: string;
  runId: string;
  keyPrefix: string;
  nodes: string[];
}

const statePath = `${LogFiles.directory}/${LogFiles.serverState}`;
const operations = [Control.drain, Control.replace] as const;

export async function publishServer(
  state: Omit<ServerState, 'controlEndpoint'>,
  control: NodeControl
): Promise<() => Promise<void>> {
  let busy = false;
  const server = createServer(async (request, response) => {
    const [command, target, extra] = request.url?.slice(1).split('/') ?? [];
    const operation = operations.find((operation) => operation === command);
    const id = state.nodes.find((id) => encodeURIComponent(id) === target);
    if (request.method !== 'POST' || !operation || !id || extra !== undefined) {
      response.writeHead(404).end();
      return;
    }
    if (busy) {
      response.writeHead(409).end('A node control operation is in progress');
      return;
    }
    busy = true;
    try {
      await control[operation](id);
      response.writeHead(204).end();
    } catch (error) {
      console.error(`[${id}] ${operation} failed`, error);
      response.writeHead(500).end(String(error));
    } finally {
      busy = false;
    }
  });
  const close = (): Promise<void> =>
    new Promise((resolve, reject) => {
      server.close((error) => (error ? reject(error) : resolve()));
    });
  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(LocalHost.controlPort, LocalHost.address, () => {
      server.off('error', reject);
      resolve();
    });
  });
  try {
    const address = server.address();
    assert(address && typeof address !== 'string');
    const controlEndpoint = `http://${LocalHost.address}:${address.port}`;
    mkdirSync(LogFiles.directory, { recursive: true });
    const temporaryPath = `${statePath}.${state.runId}`;
    writeFileSync(temporaryPath, JSON.stringify({ controlEndpoint, ...state }) + '\n', {
      flag: 'wx'
    });
    try {
      await claimStatePath(temporaryPath);
    } finally {
      unlinkSync(temporaryPath);
    }
  } catch (error) {
    await close();
    throw error;
  }
  return async () => {
    try {
      unlinkSync(statePath);
    } finally {
      await close();
    }
  };
}

export class ServerAlreadyRunningError extends Error {}

// A state file left by a server that ended without cleanup is replaced; a live one is not.
async function claimStatePath(temporaryPath: string): Promise<void> {
  try {
    linkSync(temporaryPath, statePath);
    return;
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
  }
  await assertNoRunningServer();
  unlinkSync(statePath);
  linkSync(temporaryPath, statePath);
}

export async function assertNoRunningServer(): Promise<void> {
  if (await isRecordedServerAlive()) {
    throw new ServerAlreadyRunningError(
      `A server is already running (${statePath}). Stop it with Ctrl+C in its terminal first.`
    );
  }
}

async function isRecordedServerAlive(): Promise<boolean> {
  let endpoint: string;
  try {
    endpoint = (JSON.parse(readFileSync(statePath, 'utf8')) as ServerState).controlEndpoint;
  } catch {
    return false;
  }
  try {
    await fetch(endpoint, { redirect: 'error', signal: AbortSignal.timeout(Limits.controlMs) });
    return true;
  } catch {
    return false;
  }
}

export function attachServer(): { state: ServerState; control: NodeControl } {
  const state = JSON.parse(readFileSync(statePath, 'utf8')) as ServerState;
  const endpoint = new URL(state.controlEndpoint);
  assert.equal(endpoint.protocol, 'http:');
  assert.equal(endpoint.hostname, LocalHost.address);
  assert(endpoint.port, 'Missing server control port');
  async function call(operation: (typeof operations)[number], id: string): Promise<void> {
    const response = await fetch(new URL(`/${operation}/${encodeURIComponent(id)}`, endpoint), {
      method: 'POST',
      redirect: 'error',
      signal: AbortSignal.timeout(Limits.controlMs)
    });
    if (!response.ok)
      throw new Error(`${operation} ${id} failed (${response.status}): ${await response.text()}`);
  }
  return {
    state,
    control: {
      drain: (id) => call(Control.drain, id),
      replace: (id) => call(Control.replace, id)
    }
  };
}
