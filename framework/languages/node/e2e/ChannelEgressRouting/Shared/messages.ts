export const ChannelEgressNames = {
  gameMesh: 'game',
  auditMesh: 'audit',
  session: 'game.session',
  play: 'game.play',
  api: 'game.api',
  audit: 'audit.record',
  workflow: 'workflow.command',
  spotType: 'config12.workflow-spot'
} as const;

export class ChannelProbeReq {
  constructor(
    readonly id: string,
    readonly mode = 'echo'
  ) {}
}

export interface ChannelProbeRes {
  readonly id: string;
  readonly role: string;
  readonly lifecycle?: string;
  readonly channel: string;
  readonly downstream: readonly string[];
}

export class ChannelProbeMsg {
  constructor(readonly id: string) {}
}

export class SpotWorkflowReq {
  constructor(
    readonly id: string,
    readonly timerName = `${id}-timer`
  ) {}
}

export interface SpotWorkflowRes {
  readonly id: string;
  readonly sequence: readonly string[];
}

export interface EvidenceWaitReq {
  readonly contains: string;
  readonly timeoutMilliseconds?: number;
}

export interface InvokeReq {
  readonly channel: string;
  readonly id: string;
  readonly mode?: string;
}

export interface InvokeRes {
  readonly succeeded: boolean;
  readonly error?: string;
  readonly reply?: ChannelProbeRes;
  readonly elapsedMilliseconds: number;
}

export interface SendRes {
  readonly succeeded: boolean;
  readonly error?: string;
  readonly elapsedMilliseconds: number;
}

export interface ClientServerStatusRes {
  readonly state: string;
  readonly isReady: boolean;
  readonly readyTargetCount: number;
  readonly localRole: string;
  readonly targets: readonly {
    readonly rid: string;
    readonly weight: number;
    readonly state: string;
  }[];
}

export interface RouteStatusRes {
  readonly state: string;
  readonly isReady: boolean;
  readonly readyPeerCount: number;
  readonly peers: readonly { readonly rid: string; readonly state: string }[];
  readonly channels: readonly {
    readonly channelName: string;
    readonly isReady: boolean;
    readonly readyTargetCount: number;
  }[];
}
