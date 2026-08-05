import * as fs from 'node:fs';

export interface ClientOptions {
  readonly scenario: string;
  readonly sessionUrl: string;
  readonly playUrl: string;
  readonly spotCallerUrl: string;
  readonly apiAUrl: string;
  readonly apiBUrl: string;
  readonly workflowCallerUrl: string;
  readonly workflowAUrl: string;
  readonly workflowBUrl: string;
  readonly auditUrl: string;
  readonly invalidUrl: string;
  readonly expectedWorkflowLifecycle?: string;
  readonly expectedWorkflowRid?: string;
}

export function parseClientOptions(args: readonly string[]): ClientOptions {
  if (args.length !== 2 || args[0] !== '--config') throw new Error('--config <path> is required.');
  const document = JSON.parse(fs.readFileSync(args[1], 'utf8')) as { e2e?: unknown };
  if (document.e2e === null || typeof document.e2e !== 'object' || Array.isArray(document.e2e)) {
    throw new Error('Configuration section e2e must be an object.');
  }
  const values = document.e2e as Record<string, unknown>;
  return {
    scenario: required(values, 'scenario'),
    sessionUrl: required(values, 'sessionUrl'),
    playUrl: required(values, 'playUrl'),
    spotCallerUrl: required(values, 'spotCallerUrl'),
    apiAUrl: required(values, 'apiAUrl'),
    apiBUrl: required(values, 'apiBUrl'),
    workflowCallerUrl: required(values, 'workflowCallerUrl'),
    workflowAUrl: required(values, 'workflowAUrl'),
    workflowBUrl: required(values, 'workflowBUrl'),
    auditUrl: required(values, 'auditUrl'),
    invalidUrl: required(values, 'invalidUrl'),
    expectedWorkflowLifecycle: optional(values, 'expectedWorkflowLifecycle'),
    expectedWorkflowRid: optional(values, 'expectedWorkflowRid')
  };
}

function optional(values: Record<string, unknown>, key: string): string | undefined {
  const value = values[key];
  if (value === undefined) return undefined;
  if (typeof value !== 'string' || value.length === 0) throw new Error(`e2e.${key} must be a non-empty string.`);
  return value;
}

function required(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`e2e.${key} is required.`);
  return value;
}
