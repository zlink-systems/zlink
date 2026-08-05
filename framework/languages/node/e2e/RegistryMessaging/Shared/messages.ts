import { createHash } from 'node:crypto';

export const PacketNames = {
  profileReq: 'ProfileReq',
  profileMsg: 'ProfileMsg',
  payloadReq: 'PayloadReq',
  workflowReq: 'WorkflowReq',
  scenarioRouteReq: 'ScenarioRouteReq',
  missingProfileReq: 'MissingProfileReq',
  missingProfileMsg: 'MissingProfileMsg'
} as const;

export class ProfileReq {
  constructor(readonly value: string) {}
}

export class MissingProfileReq {
  constructor(readonly value: string) {}
}

export interface ProfileRes {
  readonly value: string;
  readonly providerRid: string;
}

export class ProfileMsg {
  constructor(readonly commandId: string) {}
}

export class MissingProfileMsg {
  constructor(readonly commandId: string) {}
}

export interface EvidenceWaitReq {
  readonly contains: string;
  readonly timeoutMilliseconds?: number;
}

export class PayloadReq {
  constructor(
    readonly marker: string,
    readonly payload: string
  ) {}
}

export interface PayloadRes {
  readonly marker: string;
  readonly length: number;
  readonly sha256: string;
}

export class WorkflowReq {
  constructor(readonly value: string) {}
}

export interface WorkflowRes {
  readonly value: string;
  readonly providerRid: string;
}

export class ScenarioRouteReq {
  constructor(readonly value: string) {}
}

export interface ScenarioRouteRes {
  readonly value: string;
  readonly providerRid: string;
  readonly sourceRid: string;
}

export interface RouteMissingRes {
  readonly failed: boolean;
  readonly errorKind: string;
}

export interface TargetedRouteReq {
  readonly targetRid: string;
  readonly value: string;
}

export interface RequestOutcomeRes {
  readonly value: string;
  readonly outcome: string;
}

export interface PeerLocationWaitReq {
  readonly rid: string;
  readonly present: boolean;
  readonly timeoutMilliseconds?: number;
}

export interface RequestFailureRes {
  readonly failed: boolean;
  readonly failureType: string;
}

export function sha256Hex(value: string): string {
  return createHash('sha256').update(value, 'utf8').digest('hex').toUpperCase();
}
