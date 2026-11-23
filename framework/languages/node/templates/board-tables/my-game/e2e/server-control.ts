import assert from 'node:assert/strict';
import { linkSync, mkdirSync, readFileSync, unlinkSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { Limits, LocalHost, LogFiles } from '../server/shared/constants';

export interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
}

export interface LobbyNodeControl extends NodeControl {
  restartLobby(): Promise<string>;
}

export interface WorldNodeControl extends NodeControl {
  createUpdateZone(id: string): Promise<void>;
}

interface DeploymentState {
  controlEndpoint: string;
  runId: string;
  keyPrefix: string;
  nodes: string[];
}

export interface ServerState extends DeploymentState {
  sessionEndpoint: string;
}

export interface LobbyServerState extends ServerState {
  apiEndpoint: string;
}

export interface WorldServerState extends DeploymentState {
  sessionEndpoints: Record<string, string>;
  apiEndpoint: string;
}

const commands = {
  drain: 'drain',
  replace: 'replace',
  createUpdateZone: 'create-update-zone',
  restartLobby: 'restart-lobby',
  prepare: 'prepare'
} as const;

const controlTimeoutMs = Math.max(
  Limits.messageMs + Limits.relocationMs + Limits.safeMs,
  Limits.shutdownMs + Limits.messageMs + Limits.startupMs
);
const statePath = `${LogFiles.directory}/${LogFiles.serverState}`;
type HostControl = NodeControl & Partial<LobbyNodeControl & WorldNodeControl>;
type AttachedControl = LobbyNodeControl & WorldNodeControl;
type StateWithoutEndpoint<State extends DeploymentState> = Omit<State, 'controlEndpoint'>;

export async function publishServer<State extends DeploymentState>(
  state: StateWithoutEndpoint<State>,
  control: () => HostControl,
  prepare?: (name: string) => Promise<StateWithoutEndpoint<State>>
): Promise<() => Promise<void>> {
  let busy = false;
  const server = createServer(async (request, response) => {
    const [command, target, extra] = request.url?.slice(1).split('/') ?? [];
    const id = state.nodes.find((nodeId) => encodeURIComponent(nodeId) === target);
    const handlers = control();
    const operations = {
      [commands.drain]: id && (() => handlers.drain(id)),
      [commands.replace]: id && (() => handlers.replace(id)),
      [commands.createUpdateZone]:
        id && handlers.createUpdateZone && (() => handlers.createUpdateZone!(id)),
      [commands.restartLobby]:
        target === undefined && handlers.restartLobby && (() => handlers.restartLobby!()),
      [commands.prepare]:
        target !== undefined && prepare && (() => prepare(decodeURIComponent(target)))
    };
    const execute = Object.hasOwn(operations, command)
      ? operations[command as keyof typeof operations]
      : undefined;
    if (request.method !== 'POST' || extra !== undefined || !execute) {
      response.writeHead(404).end();
      return;
    }
    if (busy) {
      response.writeHead(409).end('A node control operation is in progress');
      return;
    }
    busy = true;
    try {
      const result = await execute();
      response.writeHead(result === undefined ? 204 : 200).end(JSON.stringify(result));
    } catch (error) {
      console.error(`Server control ${command} failed`, error);
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
    endpoint = (JSON.parse(readFileSync(statePath, 'utf8')) as DeploymentState).controlEndpoint;
  } catch {
    return false;
  }
  try {
    await fetch(endpoint, { redirect: 'error', signal: AbortSignal.timeout(controlTimeoutMs) });
    return true;
  } catch {
    return false;
  }
}

export function attachServer<State extends DeploymentState = ServerState>(): {
  state: State;
  prepare(name: string): Promise<State>;
  control: AttachedControl;
} {
  const state = JSON.parse(readFileSync(statePath, 'utf8')) as State;
  const endpoint = new URL(state.controlEndpoint);
  assert.equal(endpoint.protocol, 'http:');
  assert.equal(endpoint.hostname, LocalHost.address);
  assert(endpoint.port, 'Missing server control port');
  async function call(operation: string, id?: string): Promise<Response> {
    const response = await fetch(
      new URL(
        id === undefined ? `/${operation}` : `/${operation}/${encodeURIComponent(id)}`,
        endpoint
      ),
      {
        method: 'POST',
        redirect: 'error',
        signal: AbortSignal.timeout(controlTimeoutMs)
      }
    );
    if (!response.ok)
      throw new Error(
        `${operation} ${id ?? ''} failed (${response.status}): ${await response.text()}`
      );
    return response;
  }
  return {
    state,
    async prepare(name) {
      const deployment = (await (await call(commands.prepare, name)).json()) as State;
      return { ...deployment, controlEndpoint: state.controlEndpoint };
    },
    control: {
      async drain(id) {
        await call(commands.drain, id);
      },
      async replace(id) {
        await call(commands.replace, id);
      },
      async restartLobby() {
        return (await (await call(commands.restartLobby)).json()) as string;
      },
      async createUpdateZone(id) {
        await call(commands.createUpdateZone, id);
      }
    }
  };
}
