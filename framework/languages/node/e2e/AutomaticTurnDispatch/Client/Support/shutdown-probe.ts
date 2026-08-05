import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  ZlinkStreamException
} from '@zlink-systems/stream-connector';
import type {
  AwaitScenarioRes,
  AwaitShutdownRecoveryReq,
  AwaitShutdownScenarioReq,
  AutomaticTurnDispatchRes,
  ProbeReq
} from '../../Shared/messages';
import { AutomaticTurnDispatchNames } from '../../Shared/messages';
import type { ClientOptions } from './client-options';
import { ensure } from './scenario-assert';

export async function runShutdownWait(options: ClientOptions): Promise<void> {
  const requestId = requireOption(options.requestId, 'request-id');
  const spotId = requireOption(options.spotId, 'spot-rid');
  const scenarioId = options.shutdownScenarioId ?? 'TD-F5';
  const client = createClient(options.sessionAStreamEndpoint);
  await client.connect();
  try {
    const reply = await client.request({ requestId, spotId, delayMs: 30000 } satisfies AwaitShutdownScenarioReq)
      .packetName('AwaitShutdownScenarioReq').timeout(90000).submit<AwaitScenarioRes>();
    throw new Error(`${scenarioId} expected shutdown during a pending await, but '${reply.operation}' completed.`);
  } catch (error) {
    if (error instanceof ZlinkStreamException || isAbortLike(error)) {
      console.log(`execution-turn ${scenarioId} shutdown wait result=passed`);
      return;
    }
    throw error;
  } finally {
    await client.close();
  }
}

export async function runShutdownRecovery(options: ClientOptions): Promise<void> {
  const requestId = requireOption(options.requestId, 'request-id');
  const spotId = requireOption(options.spotId, 'spot-rid');
  const scenarioId = options.shutdownScenarioId ?? 'TD-F5';
  const client = createClient(options.sessionAStreamEndpoint);
  await client.connect();
  try {
    const result = await client.request({ requestId, spotId } satisfies AwaitShutdownRecoveryReq)
      .packetName('AwaitShutdownRecoveryReq').timeout(30000).submit<AwaitScenarioRes>();
    ensure(result.operation === 'await.e3-shutdown-recovery', `${scenarioId} recovery operation mismatch.`);
    ensure(result.spotId === spotId, `${scenarioId} recovery Spot routing id mismatch.`);
    ensure(result.evidence.some((line) => line.includes(`request=${requestId}`)
      && line.includes('marker=shutdown-recovery-probe')),
    `${scenarioId} recovery probe marker missing.`);
    console.log(`execution-turn ${scenarioId} shutdown recovery result=passed`);
  } finally {
    await client.close();
  }
}

export async function runShutdownAdmission(options: ClientOptions): Promise<void> {
  const requestId = requireOption(options.requestId, 'request-id');
  const spotId = requireOption(options.spotId, 'spot-rid');
  const scenarioId = options.shutdownScenarioId ?? 'TD-F5A';
  const client = createClient(options.sessionAStreamEndpoint);
  await client.connect();
  try {
    const request = {
      requestId,
      marker: 'shutdown-admission'
    } satisfies ProbeReq;
    const reply = await client.request(request)
      .packetName('ProbeReq')
      .metadata(AutomaticTurnDispatchNames.spotIdMetadata, spotId)
      .timeout(10000)
      .submit<AutomaticTurnDispatchRes>();
    throw new Error(`${scenarioId} accepted a new request after shutdown seal: ${reply.marker}`);
  } catch (error) {
    if (isShuttingDown(error)) {
      console.log(`execution-turn ${scenarioId} shutdown admission result=passed`);
      return;
    }
    throw error;
  } finally {
    await client.close();
  }
}

function createClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 30000,
    requestTimeoutMs: 60000,
    maxReceivedMessages: 1024
  });
}

function requireOption(value: string | undefined, name: string): string {
  if (value === undefined || value.length === 0) throw new Error(`--${name} is required for shutdown probes.`);
  return value;
}

function isAbortLike(error: unknown): boolean {
  return error instanceof Error && /abort|cancel|close|closed|disconnect|terminated/i.test(error.message);
}

function isShuttingDown(error: unknown): boolean {
  if (!(error instanceof ZlinkStreamException)) return false;
  const remote = error.error.cause;
  return typeof remote === 'object'
    && remote !== null
    && 'code' in remote
    && remote.code === 'ShuttingDown';
}
