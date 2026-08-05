import { Inject, Injectable } from '@nestjs/common';
import type {
  AuthRes,
  AuthReq,
  EnsureActorRes,
  LogicalDisconnectRes,
  LogicalDisconnectReq,
  MultiBindRes,
  MultiBindReq,
  UserSpotAuthReq
} from '../../../Shared/messages';
import {
  CreateSpotReq,
  EnsureActorReq,
  SpotServiceNames,
  spotServicePacket
} from '../../../Shared/messages';
import type {
  ZLinkActorManager,
  ZLinkMessage,
  ZLinkRouteClient,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import { ZLINK_ACTOR_MANAGER, ZLINK_ROUTE_CLIENT } from '@zlink-systems/nestjs';
import { EvidenceStore } from '../Infrastructure/evidence-store';

class ScenarioSession implements ZLinkSession {
  constructor(
    private readonly route: ZLinkRouteClient,
    private readonly actors: ZLinkActorManager,
    private readonly evidence: EvidenceStore,
    readonly context: ZLinkSessionContext
  ) {}

  async onConnected(): Promise<void> {
    this.evidence.add(`session-connected|rid=${this.evidence.rid}|session=${this.context.sessionId}`);
  }

  async onDisconnected(): Promise<void> {
    // Framework가 disconnect 시점의 exact binding snapshot 전체에 통지한다.
    // Application callback은 Actor 목록을 순회하거나 통지를 다시 제출하지 않는다.
    this.evidence.add(`session-disconnected|rid=${this.evidence.rid}|session=${this.context.sessionId}`);
  }

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage, signal?: AbortSignal): Promise<void> {
    if (dispatch.packetName === 'AuthReq') {
      const request = payload.decode<AuthReq>(Object as never);
      const meshName = request.meshName ?? SpotServiceNames.spotChannel;
      this.evidence.add(`session-auth|rid=${this.evidence.rid}|actor=${request.actorId}|step=received`);
      try {
        const ensured = await this.ensureActorInMesh(request, meshName, signal);
        this.evidence.add(`session-auth|rid=${this.evidence.rid}|actor=${request.actorId}|step=ensured`);
        await this.context.actors.bindOrGet({
          actorId: ensured.actorId,
          nodeRid: ensured.nodeRid,
          objectGeneration: BigInt(ensured.generation),
          meshName
        }, signal);
        this.evidence.add(`session-auth|rid=${this.evidence.rid}|actor=${request.actorId}|step=bound`);
        this.context.client.reply({
          actorId: ensured.actorId,
          nodeRid: ensured.nodeRid,
          generation: ensured.generation
        } satisfies AuthRes).submit();
        this.evidence.add(`session-auth|rid=${this.evidence.rid}|actor=${request.actorId}|step=replied`);
      } catch (error) {
        this.evidence.add(
          `session-auth|rid=${this.evidence.rid}|actor=${request.actorId}|step=failed|error=${error instanceof Error ? error.message : String(error)}`
        );
        throw error;
      }
      return;
    }

    if (dispatch.packetName === 'UserSpotAuthReq') {
      const request = payload.decode<UserSpotAuthReq>(Object as never);
      await this.ensureRemoteUserSpot(request, signal);
      const ensured = await this.ensureRemoteActor({
        actorId: request.actorId,
        displayName: request.displayName,
        nodeRid: request.nodeRid
      }, signal);
      await this.context.actors.bindOrGet({
        actorId: ensured.actorId,
        nodeRid: ensured.nodeRid,
        objectGeneration: BigInt(ensured.generation),
        meshName: SpotServiceNames.spotChannel
      }, signal);
      this.context.client.reply({
        actorId: ensured.actorId,
        nodeRid: ensured.nodeRid,
        generation: ensured.generation
      } satisfies AuthRes).submit();
      return;
    }

    if (dispatch.packetName === 'MultiBindReq') {
      const request = payload.decode<MultiBindReq>(Object as never);
      for (const actorId of [request.firstActorId, request.secondActorId]) {
        const ensured = await this.ensureRemoteActor({
          actorId,
          displayName: actorId,
          nodeRid: request.nodeRid
        }, signal);
        await this.context.actors.bindOrGet({
          actorId: ensured.actorId,
          nodeRid: ensured.nodeRid,
          objectGeneration: BigInt(ensured.generation),
          meshName: SpotServiceNames.spotChannel
        }, signal);
      }
      this.context.client.reply({
        boundCount: this.context.actors.bound.length
      } satisfies MultiBindRes).submit();
      return;
    }

    if (dispatch.packetName === 'LogicalDisconnectReq') {
      const request = payload.decode<LogicalDisconnectReq>(Object as never);
      const actor = this.context.actors.find(request.actorId);
      if (actor === undefined) {
        throw new Error(`Actor route not found: ${request.actorId}`);
      }
      await actor.notifyDisconnected(signal);
      this.context.client.reply({
        actorId: request.actorId,
        remainingActorIds: this.context.actors.bound.map((bound) => bound.actorId)
      } satisfies LogicalDisconnectRes).submit();
      return;
    }

    const actorId = dispatch.metadata.get(SpotServiceNames.actorIdMetadata);
    const actor = actorId === undefined || actorId.trim() === ''
      ? this.requireSingleBoundActor(dispatch.packetName)
      : this.context.actors.find(actorId);
    if (actor === undefined) {
      throw new Error(`Actor route not found: ${actorId}`);
    }
    await actor.relay(payload, signal);
  }

  private async ensureLocalActor(request: AuthReq, signal?: AbortSignal): Promise<EnsureActorRes> {
    const meshName = request.meshName ?? SpotServiceNames.spotChannel;
    const created = await this.actors
      .getOrCreate(request.actorId, SpotServiceNames.actorType)
      .inMesh(meshName)
      .request(request)
      .submit(signal);
    if (created.status === 'rejected') {
      throw new Error(`Actor '${request.actorId}' creation was rejected.`);
    }
    const actorRef = created.actor;
    this.evidence.add(`ensure-actor|rid=${this.evidence.rid}|actor=${request.actorId}`);
    this.evidence.add(`entry-joined|rid=${this.evidence.rid}|actor=${request.actorId}`);
    return {
      actorId: actorRef.actorId,
      nodeRid: String(actorRef.nodeRid),
      generation: actorRef.objectGeneration.toString()
    };
  }

  private async ensureActorInMesh(
    request: AuthReq,
    meshName: string,
    signal?: AbortSignal
  ): Promise<EnsureActorRes> {
    const created = await this.actors
      .getOrCreate(
        request.actorId,
        meshName === SpotServiceNames.spotChannel
          ? SpotServiceNames.actorType
          : SpotServiceNames.alternateActorType
      )
      .inMesh(meshName)
      .request(request)
      .submit(signal);
    if (created.status === 'rejected') {
      throw new Error(`Actor '${request.actorId}' creation was rejected.`);
    }
    return {
      actorId: created.actor.actorId,
      nodeRid: String(created.actor.nodeRid),
      generation: created.actor.objectGeneration.toString()
    };
  }

  private async ensureRemoteUserSpot(request: UserSpotAuthReq, signal?: AbortSignal): Promise<void> {
    await this.route
      .requestToNode(SpotServiceNames.controlChannel, request.nodeRid,
        spotServicePacket(CreateSpotReq, { spotId: request.spotId }))
      .timeout(5000)
      .submit(signal);
  }

  private async ensureRemoteActor(request: AuthReq, signal?: AbortSignal): Promise<EnsureActorRes> {
    return await this.route
      .requestToNode(SpotServiceNames.controlChannel, request.nodeRid,
        spotServicePacket(EnsureActorReq, {
        actorId: request.actorId,
        displayName: request.displayName,
        nodeRid: request.nodeRid,
        ...(request.meshName === undefined ? {} : { meshName: request.meshName })
        }))
      .timeout(5000)
      .submit<EnsureActorRes>(signal);
  }

  private requireSingleBoundActor(packetName: string) {
    if (this.context.actors.bound.length === 1) {
      return this.context.actors.bound[0];
    }
    if (this.context.actors.bound.length === 0) {
      throw new Error(`No actor is bound for packet '${packetName}'.`);
    }
    throw new Error(`Actor id metadata is required for packet '${packetName}' with multiple bound actors.`);
  }
}

@Injectable()
export class ScenarioSessionFactory implements ZLinkSessionFactory<ScenarioSession> {
  constructor(
    @Inject(ZLINK_ROUTE_CLIENT) private readonly route: ZLinkRouteClient,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    private readonly evidence: EvidenceStore
  ) {}

  async create(context: ZLinkSessionContext): Promise<ScenarioSession> {
    return new ScenarioSession(this.route, this.actors, this.evidence, context);
  }
}
