import fs from 'node:fs';
import path from 'node:path';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import type { ZLinkRouteClient, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type { ProfileRes, ProfileReq } from '../../Shared/messages';
import { RuntimeMonitoringNames } from '../../Shared/messages';
import { validateTriggerOptions } from './Configuration/trigger-options';
import type { TriggerOptions } from './Configuration/trigger-options';
import { MONITORING_OPTIONS, createMonitoringConfigurationModule } from '../../configuration';
import { createTriggerEndpoints, requestProfile } from './Endpoints/trigger-endpoints';
import { EvidenceStore } from '../Service/Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

interface TriggerPeer {
  readonly endpoint: string;
  readonly routingId: string;
}

export async function startTriggerHost(): Promise<void> {
  let stopping = false;

  const TriggerModule = createConfiguredTriggerModule();
  const app = await NestFactory.createApplicationContext(TriggerModule, { logger: false, abortOnError: false });
  const options = app.get(MONITORING_OPTIONS, { strict: false }) as TriggerOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(
    options.httpUrl,
    createTriggerEndpoints(options, channel, evidence, (request, endpoint, channelName) => requestWithTransientHost(options, request, endpoint, channelName), () => { stopping = true; })
  );

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createConfiguredTriggerModule(): Function {
  class TriggerModule {}
  const configuration = createMonitoringConfigurationModule(validateTriggerOptions);
  Module({
    imports: [configuration, ZLinkModule.forRootFactory({
      imports: [configuration], inject: [MONITORING_OPTIONS],
      useFactory: (value: unknown) => buildTriggerFramework(value as TriggerOptions)
    })],
    providers: [
      { provide: EvidenceStore, inject: [MONITORING_OPTIONS], useFactory: (value: unknown) => {
        const options = value as TriggerOptions; return new EvidenceStore('trigger', path.join(options.logDir, 'trigger.evidence.log'));
      } }
    ]
  })(TriggerModule);
  return TriggerModule;
}

async function requestWithTransientHost(
  options: TriggerOptions,
  request: ProfileReq,
  channelEndpoint = options.serviceChannelEndpoint,
  channelName: string = RuntimeMonitoringNames.channel
): Promise<ProfileRes> {
  const traceLabel = `trigger-${request.marker}`;
  const evidence = new EvidenceStore(traceLabel, path.join(options.logDir, `${traceLabel}.evidence.log`));
  const TriggerModule = createTriggerModule(options, evidence, traceLabel, channelEndpoint, channelName);
  const app = await NestFactory.createApplicationContext(TriggerModule, { logger: false, abortOnError: false });
  try {
    const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
    const runtime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
    await waitForTransientChannelReady(runtime, channelName);
    try {
      return await requestProfile(channel, request, channelName);
    } catch (error) {
      console.error('transient request failed', error, runtime.snapshot(channelName));
      throw error;
    }
  } finally {
    await Promise.race([
      app.close(),
      new Promise<void>((resolve) => setTimeout(resolve, 3000))
    ]);
  }
}

function createTriggerModule(
  options: TriggerOptions,
  evidence: EvidenceStore,
  traceLabel = 'trigger',
  channelEndpoint = options.serviceChannelEndpoint,
  channelName: string = RuntimeMonitoringNames.channel,
  expectedRoutingId = expectedRoutingIdForEndpoint(options, channelEndpoint)
): Function {
  class TriggerModule {}

  Module({
    imports: [
      ZLinkModule.forRootFactory({
      useFactory: () => buildTriggerFramework(
        options,
        traceLabel,
        [{ endpoint: channelEndpoint, routingId: expectedRoutingId }],
        channelName
      )
      })
    ],
    providers: [
      { provide: EvidenceStore, useValue: evidence }
    ]
  })(TriggerModule);
  return TriggerModule;
}

function buildTriggerFramework(
  options: TriggerOptions,
  traceLabel = 'trigger',
  channelEndpoints: readonly TriggerPeer[] = [{ endpoint: options.serviceChannelEndpoint, routingId: 'svc-a' }],
  channelName: string = RuntimeMonitoringNames.channel
) {
  fs.mkdirSync(options.logDir, { recursive: true });
  const builder = zlinkFramework();
  builder.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
    .traceLogFile(`${options.logDir}/${traceLabel}-flow.log`).traceLabel(traceLabel);
  const serviceMesh = builder.addRouteMesh(channelName)
    .listen('tcp://127.0.0.1:0')
    .routingId(traceLabel);
  serviceMesh.channel(channelName).client();
  for (const peer of channelEndpoints) {
    serviceMesh.peerConnections().connect(peer.routingId, peer.endpoint);
  }
  return builder.build();
}

function expectedRoutingIdForEndpoint(options: TriggerOptions, endpoint: string): string {
  if (endpoint === options.serviceBChannelEndpoint || endpoint === options.replacementServiceChannelEndpoint) {
    return 'svc-b';
  }
  if (endpoint === options.throwChannelEndpoint) return 'svc-throw';
  return 'svc-a';
}

async function waitForTransientChannelReady(runtime: ZLinkRouteMeshRuntime, meshName: string): Promise<void> {
  const deadline = Date.now() + 10000;
  let lastStatus: unknown;
  while (Date.now() < deadline) {
    try {
      const status = runtime.snapshot(meshName);
      lastStatus = {
        state: status.state,
        isReady: status.isReady,
        readyPeerCount: status.readyPeerCount,
        peers: status.peers,
        channels: status.channels
      };
      if (status.isReady) {
        return;
      }
    } catch {
      // The runtime may not have published its first status yet.
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  console.error('transient readiness status:', lastStatus);
  throw new Error('Timed out waiting for transient trigger channel readiness.');
}
