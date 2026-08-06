export interface InstanceScenarioContext {
  readonly callerUrl: string;
  readonly ownerUrl: string;
  readonly ownerRid: string;
  readonly scenarioId: string;
}

export interface InstanceEvidence {
  readonly entered: number;
  readonly completed: number;
  readonly instanceCount: number;
}

interface InstanceReply {
  readonly status: string;
  readonly spotId: string;
  readonly operationId: string;
  readonly action: string;
  readonly instanceSpot?: boolean;
}

export async function requestInstance(
  context: InstanceScenarioContext,
  spotId: string,
  operationId: string,
  action: string
): Promise<InstanceReply> {
  const reply = await postJson<InstanceReply>(context.callerUrl, '/instance/request', {
    spotId,
    operationId,
    action
  });
  ensure(reply.status === 'completed', `${context.scenarioId}: request did not complete`);
  ensure(reply.spotId === spotId, `${context.scenarioId}: Spot identity changed`);
  ensure(reply.operationId === operationId, `${context.scenarioId}: operation identity changed`);
  ensure(reply.instanceSpot === true, `${context.scenarioId}: reply was not handled by an Instance Spot`);
  return reply;
}

export async function sendInstance(
  context: InstanceScenarioContext,
  spotId: string,
  operationId: string,
  action: string
): Promise<void> {
  const reply = await postJson<{ readonly status: string; readonly spotId: string; readonly operationId: string }>(
    context.callerUrl,
    '/instance/send',
    { spotId, operationId, action }
  );
  ensure(reply.status === 'accepted', `${context.scenarioId}: send was not accepted`);
  ensure(reply.spotId === spotId, `${context.scenarioId}: send Spot identity changed`);
  ensure(reply.operationId === operationId, `${context.scenarioId}: send operation identity changed`);
}

export async function runConcurrentRequests(
  context: InstanceScenarioContext,
  spotId: string,
  count: number,
  action: string
): Promise<readonly string[]> {
  const operationIds = Array.from({ length: count }, (_, index) =>
    `${context.scenarioId.toLowerCase()}-${Date.now()}-${index}`
  );
  await Promise.all(operationIds.map((operationId) =>
    requestInstance(context, spotId, operationId, action)
  ));
  return operationIds;
}

export async function waitForInstanceEvidence(
  context: InstanceScenarioContext,
  operationId: string
): Promise<InstanceEvidence> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const evidence = await getJson<InstanceEvidence>(
      context.ownerUrl,
      `/evidence?operationId=${encodeURIComponent(operationId)}`
    );
    if (evidence.completed === 1) {
      ensure(evidence.entered === 1, `${context.scenarioId}: handler entered more than once`);
      return evidence;
    }
    await delay(50);
  }
  throw new Error(`${context.scenarioId}: evidence did not converge for ${operationId}`);
}

export async function waitForReady(context: InstanceScenarioContext): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const status = await getStatus(
      `${context.callerUrl}/ready?targetRid=${encodeURIComponent(context.ownerRid)}`
    );
    if (status >= 200 && status < 300) return;
    await delay(50);
  }
  throw new Error(`${context.scenarioId}: Instance Spot route did not become ready`);
}

export function ensure(condition: boolean, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

async function getJson<T>(baseUrl: string, path: string): Promise<T> {
  return await httpGetJson<T>(baseUrl, path);
}

async function postJson<T>(baseUrl: string, path: string, body: unknown): Promise<T> {
  return await httpPostJson<T>(baseUrl, path, body);
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
import { getJson as httpGetJson, getStatus, postJson as httpPostJson } from '../../../http-client';
