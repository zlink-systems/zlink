import { getJson } from '../../../http-client';

export interface PublicPeerStatus {
  readonly nodeRid: string;
  readonly state: number;
  readonly unavailableReason?: number;
}

export interface PublicRouteStatus {
  readonly meshName: string;
  readonly state: number;
  readonly isReady: boolean;
  readonly readyPeerCount: number;
  readonly channels: readonly {
    readonly channelName: string;
    readonly isReady: boolean;
    readonly readyTargetCount: number;
  }[];
  readonly peers: readonly PublicPeerStatus[];
  readonly placement: {
    readonly isAvailable: boolean;
    readonly activeActorCount: number;
    readonly activeSpotCount: number;
    readonly unavailableReason?: number;
  };
  readonly sequence: string;
  readonly observedAt: string;
}

export interface PublicHostStatus {
  readonly state: number;
  readonly isReady: boolean;
  readonly acceptingWork: boolean;
  readonly inboundDispatch: {
    readonly applicationHwmBytes: string;
    readonly pendingPayloadBytes: string;
    readonly queuedPayloadBytes: string;
    readonly activePayloadBytes: string;
    readonly applicationReceivePaused: boolean;
  };
  readonly sequence: string;
  readonly observedAt: string;
}

export interface PublicObservedRoute {
  readonly status: PublicRouteStatus;
  readonly loss: {
    readonly coalescedCount: string;
    readonly discardedTerminalCount: string;
  };
}

export async function readRouteStatus(url: string): Promise<PublicRouteStatus> {
  return await readRouteStatusAt(url, '/status/route');
}

export async function readRouteStatusAt(url: string, path: string): Promise<PublicRouteStatus> {
  return await getJson<PublicRouteStatus>(url, path);
}

export async function readHostStatus(url: string): Promise<PublicHostStatus> {
  return await getJson<PublicHostStatus>(url, '/status/host');
}

export function routeStatusesFromEvidence(lines: readonly string[]): PublicRouteStatus[] {
  return lines.flatMap((line) => {
    const value = line.startsWith('public-status|route=')
      ? line.slice('public-status|route='.length)
      : line.startsWith('public-observed|route=')
        ? line.slice('public-observed|route='.length)
        : undefined;
    if (value === undefined) return [];
    try {
      const parsed = JSON.parse(value) as PublicRouteStatus | PublicObservedRoute;
      return 'status' in parsed ? [parsed.status] : [parsed];
    } catch {
      return [];
    }
  });
}

export function hostStatusesFromEvidence(lines: readonly string[]): PublicHostStatus[] {
  return lines.flatMap((line) => {
    const value = line.startsWith('public-status|host=')
      ? line.slice('public-status|host='.length)
      : line.startsWith('public-observed|host=')
        ? line.slice('public-observed|host='.length)
        : undefined;
    if (value === undefined) return [];
    try {
      const parsed = JSON.parse(value) as PublicHostStatus | { readonly status: PublicHostStatus };
      return 'status' in parsed ? [parsed.status] : [parsed];
    } catch {
      return [];
    }
  });
}

export async function waitForRouteStatus(
  url: string,
  predicate: (status: PublicRouteStatus) => boolean,
  message: string,
  timeoutMs = 20_000
): Promise<PublicRouteStatus> {
  return await waitForRouteStatusAt(url, '/status/route', predicate, message, timeoutMs);
}

export async function waitForRouteStatusAt(
  url: string,
  path: string,
  predicate: (status: PublicRouteStatus) => boolean,
  message: string,
  timeoutMs = 20_000
): Promise<PublicRouteStatus> {
  const deadline = Date.now() + timeoutMs;
  let last: PublicRouteStatus | undefined;
  while (Date.now() < deadline) {
    try {
      last = await readRouteStatusAt(url, path);
      if (predicate(last)) return last;
    } catch {
      // The endpoint can be temporarily unavailable while its host restarts.
    }
    await delay(100);
  }
  throw new Error(`${message}${last === undefined ? '' : ` Last sequence=${last.sequence}.`}`);
}

export async function waitForHostStatus(
  url: string,
  predicate: (status: PublicHostStatus) => boolean,
  message: string,
  timeoutMs = 20_000
): Promise<PublicHostStatus> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const status = await readHostStatus(url);
      if (predicate(status)) return status;
    } catch {
      // The endpoint can be temporarily unavailable while its host restarts.
    }
    await delay(100);
  }
  throw new Error(message);
}

export function delay(milliseconds = 100): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
