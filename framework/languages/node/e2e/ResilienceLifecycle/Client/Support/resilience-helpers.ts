import type { ProfileRes, ProfileReq } from '../../Shared/messages';
import { getJson, getStatus, postJson } from '../../../http-client';
import { ensure } from './scenario-assert';

interface PeerLocationResult {
  readonly routingId?: string;
  readonly rid?: string;
  readonly endpoint?: string;
}

export function profileReq(marker: string): ProfileReq {
  return { value: 'fast', marker };
}

export async function sendRequestBatch(
  consumerUrl: string,
  markerPrefix: string,
  expectedProviderRid?: string
): Promise<boolean> {
  let sawExpectedProvider = expectedProviderRid === undefined;
  for (let i = 0; i < 32; i += 1) {
    const marker = `${markerPrefix}-${i}`;
    const reply = await postJson<ProfileRes>(consumerUrl, '/profile/request', profileReq(marker));
    ensure(reply.value === 'profile:fast', `${markerPrefix} request returned an unexpected value.`);
    if (expectedProviderRid !== undefined && reply.providerRid === expectedProviderRid) {
      sawExpectedProvider = true;
    }
  }
  return sawExpectedProvider;
}

export async function waitForProviderTraffic(
  consumerUrl: string,
  markerPrefix: string,
  expectedProviderRid: string
): Promise<void> {
  const deadline = Date.now() + 30000;
  let batch = 0;
  while (Date.now() < deadline) {
    if (await sendRequestBatch(consumerUrl, `${markerPrefix}-${batch++}`, expectedProviderRid)) {
      return;
    }
    await delay(250);
  }
  throw new Error(`${markerPrefix} traffic did not reach ${expectedProviderRid}.`);
}

export async function waitForAnyProviderTraffic(
  consumerUrl: string,
  markerPrefix: string
): Promise<void> {
  const deadline = Date.now() + 30000;
  let attempt = 0;
  while (Date.now() < deadline) {
    try {
      const reply = await postJson<ProfileRes>(
        consumerUrl,
        '/profile/request',
        profileReq(`${markerPrefix}-${attempt++}`)
      );
      if (reply.value === 'profile:fast') return;
    } catch {
      // Health readiness precedes RouteMesh discovery and channel admission.
    }
    await delay(250);
  }
  const descriptors = await getJson<PeerLocationResult[]>(
    consumerUrl,
    '/location/peers'
  ).catch(() => []);
  throw new Error(
    `${markerPrefix} traffic did not reach a Ready provider;`
    + ` descriptors=${JSON.stringify(descriptors)}`
  );
}

export async function waitPeerPresent(peerLocationUrl: string, rid: string): Promise<void> {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const entries = await getJson<PeerLocationResult[]>(peerLocationUrl, '/location/peers');
    if (entries.some((entry) => entry.routingId === rid || entry.rid === rid)) {
      return;
    }
    await delay(250);
  }
  throw new Error(`Timed out waiting for peer rid=${rid} to appear.`);
}

export async function waitPeerEndpointPresent(peerLocationUrl: string, rid: string, endpoint: string): Promise<void> {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const entries = await getJson<PeerLocationResult[]>(peerLocationUrl, '/location/peers');
    if (entries.some((entry) => (entry.routingId === rid || entry.rid === rid) && entry.endpoint === endpoint)) {
      return;
    }
    await delay(250);
  }
  throw new Error(`Timed out waiting for peer rid=${rid} endpoint=${endpoint} to appear.`);
}

export async function waitPeerAbsent(peerLocationUrl: string, rid: string): Promise<void> {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const entries = await getJson<PeerLocationResult[]>(peerLocationUrl, '/location/peers');
    if (!entries.some((entry) => entry.routingId === rid || entry.rid === rid)) {
      return;
    }
    await delay(250);
  }
  throw new Error(`Timed out waiting for peer rid=${rid} to disappear.`);
}

export async function waitPeerEndpointAbsent(peerLocationUrl: string, endpoint: string): Promise<void> {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const entries = await getJson<PeerLocationResult[]>(peerLocationUrl, '/location/peers');
    if (!entries.some((entry) => entry.endpoint === endpoint)) {
      return;
    }
    await delay(250);
  }
  throw new Error(`Timed out waiting for peer endpoint=${endpoint} to disappear.`);
}

export async function waitUntilAvailable(baseUrl: string): Promise<void> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    try {
      const status = await getStatus(`${baseUrl}/health`);
      if (status >= 200 && status < 300) {
        return;
      }
    } catch {
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for provider health: ${baseUrl}`);
}

export async function waitUntilDown(baseUrl: string): Promise<void> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    try {
      const status = await getStatus(`${baseUrl}/health`);
      if (status < 200 || status >= 300) {
        return;
      }
    } catch {
      return;
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for provider shutdown: ${baseUrl}`);
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
