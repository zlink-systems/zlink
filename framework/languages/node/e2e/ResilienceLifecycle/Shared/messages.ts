import { createHash } from 'node:crypto';

export const PacketNames = {
  profileReq: 'ProfileReq',
  profileMsg: 'ProfileMsg',
  payloadReq: 'PayloadReq',
  missingProfileReq: 'MissingProfileReq',
  missingProfileMsg: 'MissingProfileMsg',
  loadEvent: 'LoadEvent'
} as const;

export const ResilienceNames = {
  fanoutChannel: 'resilience.events',
  loadTopic: 'load'
} as const;

export class ProfileReq {
  constructor(
    readonly value: string,
    readonly marker?: string
  ) {}
}

export interface ProfileRes {
  readonly value: string;
  readonly providerRid: string;
}

export class ProfileMsg {
  constructor(readonly commandId: string) {}
}

export class LoadEvent {
  constructor(
    readonly runId: string,
    readonly sequence: number
  ) {}
}

export class MissingProfileReq {
  constructor(
    readonly value: string,
    readonly marker?: string
  ) {}
}

export class MissingProfileMsg {
  constructor(readonly commandId: string) {}
}

export interface EvidenceWaitReq {
  readonly contains: string;
  readonly timeoutMilliseconds?: number;
}

export interface WeightWaitReq {
  readonly expected: number;
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

export interface RouteMissingRes {
  readonly failed: boolean;
}

export interface RequestFailureRes {
  readonly failed: boolean;
  readonly failureType: string;
  readonly failureMessage: string;
}

export interface TimeoutRes {
  readonly status: number;
  readonly timedOut: boolean;
}

export function sha256Hex(value: string): string {
  return createHash('sha256').update(value, 'utf8').digest('hex').toUpperCase();
}
