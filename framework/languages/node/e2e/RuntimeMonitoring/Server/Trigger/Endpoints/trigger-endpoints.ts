import net from 'node:net';
import fs from 'node:fs';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { ProfileReq, type EvidenceWaitReq, type ProfileRes } from '../../../Shared/messages';
import { RuntimeMonitoringNames } from '../../../Shared/messages';
import type { TriggerOptions } from '../Configuration/trigger-options';
import type { HttpRoute } from '../Support/http-server';
import type { EvidenceStore } from '../../Service/Infrastructure/evidence-store';

export function createTriggerEndpoints(
  options: TriggerOptions,
  channel: ZLinkRouteClient,
  evidence: EvidenceStore,
  requestWithTransientHost: (request: ProfileReq, endpoint?: string, channelName?: string) => Promise<ProfileRes>,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready' }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
        return evidence.waitUntil((entries) =>
          request.containsAll.every((expected) => entries.some((entry) => entry.includes(expected)))
          && request.containsAnyGroups.every((group) => group.some((expected) =>
            entries.some((entry) => entry.includes(expected)))), timeout);
      }
    },
    { method: 'POST', path: '/profile/request', handle: (body) => requestProfile(channel, toProfileReq(body)) },
    { method: 'POST', path: '/profile/request/disconnect', handle: (body) => requestWithTransientHost(toProfileReq(body)) },
    {
      method: 'POST',
      path: '/profile/request/service-b',
      handle: (body) => requestWithTransientHost(toProfileReq(body), options.serviceBChannelEndpoint)
    },
    {
      method: 'POST',
      path: '/profile/request/replacement',
      handle: (body) => requestWithTransientHost(toProfileReq(body), options.replacementServiceChannelEndpoint)
    },
    {
      method: 'POST',
      path: '/profile/request/throw',
      handle: (body) => requestWithTransientHost(toProfileReq(body), options.throwChannelEndpoint, RuntimeMonitoringNames.throwChannel)
    },
    { method: 'POST', path: '/socket/handshake-failure', handle: async () => {
      await sendInvalidHandshake(options.serviceChannelEndpoint);
      return { triggered: true };
    } },
    { method: 'GET', path: '/logs/throw-stderr', handle: () => readLines(`${options.logDir}/svc-throw.stderr.log`) },
    {
      method: 'POST',
      path: '/logs/throw-stderr/wait',
      handle: (body) => waitForLines(`${options.logDir}/svc-throw.stderr.log`, body as EvidenceWaitReq)
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping', logDir: options.logDir }; } }
  ];
}

function toProfileReq(body: unknown): ProfileReq {
  const request = body as ProfileReq;
  return new ProfileReq(request.value, request.marker);
}

export async function requestProfile(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  channelName: string = RuntimeMonitoringNames.channel
): Promise<ProfileRes> {
  return await channel
    .requestToChannel(channelName, request)
    .timeout(10000)
    .submit<ProfileRes>();
}

async function sendInvalidHandshake(endpoint: string): Promise<void> {
  const { host, port } = parseTcpEndpoint(endpoint);
  await new Promise<void>((resolve, reject) => {
    const socket = net.createConnection({ host, port }, () => {
      socket.write('not-a-zlink-handshake', () => {
        setTimeout(() => {
          socket.end();
          resolve();
        }, 500);
      });
    });
    socket.setTimeout(2000);
    socket.once('timeout', () => {
      socket.destroy();
      reject(new Error(`Timed out writing invalid handshake to ${endpoint}.`));
    });
    socket.once('error', reject);
  });
}

function parseTcpEndpoint(endpoint: string): { host: string; port: number } {
  const url = new URL(endpoint);
  if (url.protocol !== 'tcp:') {
    throw new Error(`Unsupported endpoint protocol '${url.protocol}'.`);
  }
  return { host: url.hostname, port: Number(url.port) };
}

async function waitForLines(path: string, request: EvidenceWaitReq): Promise<string[]> {
  const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
  const deadline = Date.now() + timeout;
  while (Date.now() <= deadline) {
    const lines = readLines(path);
    if (
      request.containsAll.every((expected) => lines.some((line) => line.includes(expected)))
      && request.containsAnyGroups.every((group) => group.some((expected) =>
        lines.some((line) => line.includes(expected))))
    ) {
      return lines;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('Timed out waiting for runtime monitoring log evidence.');
}

function readLines(path: string): string[] {
  return fs.existsSync(path) ? fs.readFileSync(path, 'utf8').split(/\r?\n/).filter((line) => line.length > 0) : [];
}
