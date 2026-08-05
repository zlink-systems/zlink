import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkPeerState,
  type ZLinkRouteClient,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import { ScenarioRouteReq, type ScenarioRouteRes } from '../../../Shared/messages';
import type { HttpRoute } from '../../Provider/Support/http-server';

const meshName = 'registry.messaging.rm-a3';

export function createObjectClientEndpoints(
  rid: string,
  runtime: ZLinkRouteMeshRuntime,
  route: ZLinkRouteClient,
  serverWeight: number | undefined,
  stop: () => void
): HttpRoute[] {
  return [
    {
      method: 'GET',
      path: '/health',
      handle: () => ({ status: 'ready', role: 'object-client', rid })
    },
    {
      method: 'GET',
      path: '/rm-a3/status',
      handle: () => {
        const snapshot = runtime.snapshot(meshName);
        return {
          rid,
          state: snapshot.state,
          readyPeerCount: snapshot.peers.filter((peer) => peer.state === ZLinkPeerState.Ready).length,
          channels: snapshot.channels.map((channel) => ({
            channelName: channel.channelName,
            isReady: channel.isReady,
            readyTargetCount: channel.readyTargetCount,
            localWeight: serverWeight ?? 100
          })),
          peers: snapshot.peers.map((peer) => ({
            rid: String(peer.nodeRid),
            state: peerStateName(peer.state),
            ready: peer.state === ZLinkPeerState.Ready
          }))
        };
      }
    },
    {
      method: 'POST',
      path: '/rm-a3/node-direct',
      handle: async (body) => {
        const targetRid = String((body as { targetRid?: unknown }).targetRid ?? '');
        const send = await nodeDirectOutcome(async () => {
          await route
            .sendToNode(meshName, targetRid, new ScenarioRouteReq('rm-a3-send'))
            .submit();
        });
        const request = await nodeDirectOutcome(async () => {
          await route
            .requestToNode(meshName, targetRid, new ScenarioRouteReq('rm-a3-request'))
            .timeout(500)
            .submit<ScenarioRouteRes>();
        });
        return { send, request };
      }
    },
    {
      method: 'POST',
      path: '/shutdown',
      handle: () => {
        stop();
        return { status: 'stopping' };
      }
    }
  ];
}

async function nodeDirectOutcome(operation: () => Promise<void>): Promise<{
  readonly terminal: string;
  readonly errorKind: string;
}> {
  try {
    await operation();
    return { terminal: 'UnexpectedSuccess', errorKind: '' };
  } catch (error) {
    const kind = error instanceof ZLinkFrameworkException ? String(error.kind) : 'Error';
    return {
      terminal: error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.NotFound
        ? 'NotFound'
        : 'Failed',
      errorKind: kind
    };
  }
}

function peerStateName(state: ZLinkPeerState): string {
  switch (state) {
    case ZLinkPeerState.Connecting: return 'connecting';
    case ZLinkPeerState.Ready: return 'ready';
    case ZLinkPeerState.Draining: return 'draining';
    case ZLinkPeerState.NotConnected: return 'not_connected';
    case ZLinkPeerState.NotRequired: return 'not_required';
  }
}
