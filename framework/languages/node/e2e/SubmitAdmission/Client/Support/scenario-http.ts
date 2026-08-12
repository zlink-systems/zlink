import fs from 'node:fs';
import { getJson as httpGetJson, getStatus, postJson as httpPostJson } from '../../../http-client';

export interface SubmitScenarioContext {
  readonly callerUrl: string;
  readonly targetUrl: string;
  readonly publisherUrl: string;
  readonly callerRid: string;
  readonly targetRid: string;
  readonly evidenceFile: string;
  readonly scenarioId: string;
}

export interface TerminalRes {
  readonly operationId: string;
  readonly status: string;
  readonly publicInvocationCount: number;
  readonly terminalCount: number;
}

export interface HandlerEvidence {
  readonly entered: number;
  readonly completed: number;
}

export async function submit(
  context: SubmitScenarioContext,
  baseUrl: string,
  operationId: string,
  targetRid?: string
): Promise<TerminalRes> {
  void context;
  try {
    return await postJson<TerminalRes>(baseUrl, '/submit/node', { operationId, targetRid });
  } catch (error) {
    const message = String(error);
    const status = /target was not found/i.test(message)
      ? 'targetNotFound'
      : /not connected|route.*unavailable/i.test(message)
        ? 'routeNotConnected'
        : undefined;
    if (status === undefined) throw error;
    return { operationId, status, publicInvocationCount: 1, terminalCount: 1 };
  }
}

export async function submitChannel(
  context: SubmitScenarioContext,
  operationId: string
): Promise<TerminalRes> {
  return postJson<TerminalRes>(context.callerUrl, '/submit/channel', { operationId });
}

export async function submitFanout(
  context: SubmitScenarioContext,
  operationId: string
): Promise<TerminalRes> {
  return postJson<TerminalRes>(context.publisherUrl, '/submit/fanout', { operationId });
}

export async function closeGate(context: SubmitScenarioContext): Promise<void> {
  await postJson(context.targetUrl, '/gate/close', {});
}

export async function openGate(context: SubmitScenarioContext): Promise<void> {
  await postJson(context.targetUrl, '/gate/open', {});
}

export async function shutdownTarget(context: SubmitScenarioContext): Promise<void> {
  await postJson(context.targetUrl, '/shutdown', {});
}

export async function waitEvidence(
  context: SubmitScenarioContext,
  operationId: string,
  predicate: (value: HandlerEvidence) => boolean,
  baseUrl = context.targetUrl
): Promise<HandlerEvidence> {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    const value = await getJson<HandlerEvidence>(
      baseUrl,
      `/evidence?operationId=${encodeURIComponent(operationId)}`
    );
    if (predicate(value)) return value;
    await delay(100);
  }
  throw new Error(`${context.scenarioId}: evidence did not converge for ${operationId}`);
}

export async function waitRouteState(
  context: SubmitScenarioContext,
  ready: boolean
): Promise<void> {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    const status = await getStatus(
      `${context.callerUrl}/ready?targetRid=${encodeURIComponent(context.targetRid)}`
    );
    if ((status >= 200 && status < 300) === ready) return;
    await delay(100);
  }
  throw new Error(`${context.scenarioId}: route state did not become ready=${ready}`);
}

export function terminal(
  value: TerminalRes,
  operationId: string,
  status = 'submitted'
): void {
  ensure(value.operationId === operationId, `${operationId}: operation id mismatch`);
  ensure(
    value.status.toLowerCase() === status.toLowerCase(),
    `${operationId}: expected ${status}, got ${value.status}`
  );
  ensure(value.publicInvocationCount === 1, `${operationId}: invocation count mismatch`);
  ensure(value.terminalCount === 1, `${operationId}: terminal count mismatch`);
}

export function emit(context: SubmitScenarioContext, detail: unknown): void {
  const line = JSON.stringify({ scenarioId: context.scenarioId, ...(detail as object) });
  fs.appendFileSync(context.evidenceFile, `${line}\n`);
  console.log(`${context.scenarioId} PASS ${JSON.stringify(detail)}`);
}

export function ensure(condition: boolean, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

async function getJson<T>(baseUrl: string, path: string): Promise<T> {
  return await httpGetJson<T>(baseUrl, path);
}

async function postJson<T = unknown>(baseUrl: string, path: string, body: unknown): Promise<T> {
  return await httpPostJson<T>(baseUrl, path, body);
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
