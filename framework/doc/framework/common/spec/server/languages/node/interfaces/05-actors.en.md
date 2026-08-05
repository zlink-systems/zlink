# Node.js Actor And Session Binding Public Interface

A Spot relocation, including an Actor bound to a session, restores the
Actor and queue on the target, commits owner and membership, and then
starts message processing. The target runtime sends
`sessionActorLocationUpdateReqMsg` to update the binding route and the
bound-session current Actor location snapshot. Even without a response,
Actor processing doesn't stop, and the same request is resent at a fixed
interval. The snapshot provides the target MeshName/NodeRid. Since
relocation itself isn't a physical/logical disconnect, it doesn't run
the Actor disconnect callback. The route and physical connection of a
different Actor not included in the relocation target aren't changed.

[Interface table of contents](README.en.md) · [Actor Model](../../../../14-actor-model.en.md) ·
[Spot/Actor Membership](../../../../15-spot-actor.en.md)

This document fixes the exact TypeScript declarations related to Actor
that `@zlink-systems/framework` and `@zlink-systems/nestjs` export in
ZLink Framework.

## 1. Actor Identity, Factory, And Context

`ActorId` is a logical ID unique across the whole Location Store
transaction domain. Its UTF-8 encoded size is 1..255 bytes, it's
compared as a case-sensitive exact value, and it isn't normalized. A
regular message only takes `ActorId` and resolves current authority.
`ActorRef` is the immutable location snapshot used to change an exact
incarnation or bind to a session.

```ts
export interface ZLinkActor {
    readonly context: ZLinkActorContext;
    configure?(): void;
    onJoinCompleted?(completion: ZLinkActorJoinCompletion): Promise<void>;
}

export interface ZLinkActorContext {
    readonly actorId: ActorId;
    readonly objectGeneration: bigint;
    readonly meshName: string;
    readonly spotId?: SpotId;
    readonly boundSession: ZLinkBoundSession;
    joinSpot(spotId: SpotId): ZLinkActorJoinSpotCall;
    joinSpot(spotId: SpotId, request: unknown): ZLinkActorJoinSpotCall;
    joinEntrySpot(): ZLinkActorJoinEntrySpotCall;
    joinEntrySpot(request: unknown): ZLinkActorJoinEntrySpotCall;
}

export interface ZLinkActorFactory<TActor extends ZLinkActor = ZLinkActor> {
    create(context: ZLinkActorContext, signal?: AbortSignal): Promise<TActor>;
}

export interface ZLinkActorHandlerRegistry {
    addHandler<THandler>(handlerType: Type<THandler>, packetName?: string): this;
}

export interface ZLinkActorJoinCall<TSelf> {
    timeout(timeoutMs: number): TSelf;
    defer(): void;
}

export interface ZLinkActorJoinEntrySpotCall
    extends ZLinkActorJoinCall<ZLinkActorJoinEntrySpotCall> {}

export interface ZLinkActorJoinSpotCall
    extends ZLinkActorJoinCall<ZLinkActorJoinSpotCall> {}

export interface ZLinkActorJoinOperationId {
    readonly high: bigint;
    readonly low: bigint;
}

export type ZLinkActorJoinCompletion =
    | { readonly status: 'accepted'; readonly operationId: ZLinkActorJoinOperationId;
        readonly actor: ActorRef; readonly reply?: ZLinkMessage }
    | { readonly status: 'rejected'; readonly operationId: ZLinkActorJoinOperationId;
        readonly reply?: ZLinkMessage }
    | { readonly status: 'failed'; readonly operationId: ZLinkActorJoinOperationId;
        readonly kind: ZLinkFrameworkErrorKind };
```

The canonical declaration of `ActorId` and `ActorRef` is owned by
[Foundation Types And Configuration](01-foundation-configuration.en.md).
This document only fixes the exact location where the Actor lifecycle
and manager use that type.

## 2. Global Client And Manager

```ts
export interface ZLinkActorClient {
    sendToActor(actorId: ActorId, message: unknown): ZLinkActorSendCall;
    requestToActor(actorId: ActorId, request: unknown): ZLinkActorRequestCall;
}

export interface ZLinkActorManager {
    create(actorId: ActorId, actorType: string): ZLinkActorCreateCall;
    getOrCreate(actorId: ActorId, actorType: string): ZLinkActorGetOrCreateCall;
    find(actorId: ActorId, signal?: AbortSignal): Promise<ActorRef | undefined>;
    findSpot(actorId: ActorId, signal?: AbortSignal): Promise<SpotRef | undefined>;
    destroy(actor: ActorRef, signal?: AbortSignal): Promise<boolean>;
}

export interface ZLinkActorCreateCall {
    inMesh(meshName: string): this;
    request(request: unknown): this;
    timeout(timeoutMs: number): this;
    submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
    yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
}

export interface ZLinkActorGetOrCreateCall {
    inMesh(meshName: string): this;
    request(request: unknown): this;
    timeout(timeoutMs: number): this;
    submit(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
    yield(signal?: AbortSignal): Promise<ZLinkActorCreateResult>;
}

export type ZLinkActorCreateResult =
    | { readonly status: 'existing'; readonly actor: ActorRef }
    | {
        readonly status: 'created';
        readonly actor: ActorRef;
        readonly reply?: unknown;
      }
    | { readonly status: 'rejected'; readonly reply?: unknown };

export interface ZLinkActorRequestCall {
    metadata(key: string, value: string): this;
    timeout(timeoutMs: number): this;
    submit<TReply>(signal?: AbortSignal): Promise<TReply>;
    yield<TReply>(signal?: AbortSignal): Promise<TReply>;
}

export interface ZLinkActorSendCall {
    metadata(key: string, value: string): this;
    submit(signal?: AbortSignal): Promise<void>;
}
```

The call `create` and `getOrCreate` return is single-use. Setting the
same option twice, or calling terminal `submit(...)` twice, is
`InvalidOperation`. If `inMesh(...)` is omitted and there are two or
more eligible Meshes, `InvalidOperation`; if there's no object-role Mesh
at all, `NotConfigured`. If the specified Mesh doesn't exist, `NotFound`.
The caller doesn't specify a target RID or predicate.

`create` returns `AlreadyExists` if a ready incarnation of the same
ActorId exists, and `TypeMismatch` if stable type differs. A new attempt
returns `created` or `rejected`. `getOrCreate` returns a
[ready](../../../../01-glossary.en.md#ready) Actor of the same type as
`existing`, without a callback. If Creating, it waits for the authority
change, and a CAS loser doesn't start a separate factory or callback. A
different operation receives `existing` after ready, competes for a new
reservation after cleanup, and doesn't share an earlier application
reply. Only a resend of the same source Node RID/lifecycle
generation/`OperationId` reads the correlation-free
`creation-operation-terminal-v1` envelope and re-encodes the reply with
the current correlation/reply route. The terminal is kept for 5 minutes
after the original deadline. A callback exception isn't `rejected` —
it's a typed creation failure. If the whole deadline ends,
`DeadlineExceeded`; if there's no capacity, `CapacityExceeded`. An exact
lifecycle operation whose ActorRef's object generation differs from
current is `InvalidOperation`, and `Unavailable` while moving.

Actor create finishes the selected owner MeshNode's Entry Spot
membership and the Ready barrier in the same lifecycle. After Ready, a
one-way message is submitted directly to the Actor queue. Even if a
stale route is confirmed after resolve or queue admission, the
framework doesn't find a new [owner](../../../../01-glossary.en.md#owner)
and hidden-retry the same operation.

The Actor Join call only provides a synchronous `defer()`, and doesn't
provide `submit(...)`/`yield(...)`. `defer()` only registers an
immutable Join intent and an inactive barrier on the current handler,
and doesn't start a target lookup or Store I/O. If the handler finishes
normally, the Join runs; if it fails, the barrier is discarded. If the
handler used `yield(...)`, the barrier isn't activated until the last
continuation finishes.

The result is delivered via the `onJoinCompleted(...)` Actor callback
with the same operation ID. Operation ID is a completion idempotency
ID, not a `RelocationId`, reservation ID, or aggregate commit ID.
Same-node and cross-node completion retry are limited to the current
source and target process lifetime. After the process ends, a different
runtime doesn't automatically replay completion.

The overload with no request fixes an empty `ZLinkMessage`. The default
timeout is 5 seconds, and an explicit value is a finite
`1..2_147_483_647` ms rounded up to milliseconds. The monotonic
absolute deadline is fixed at the moment `defer()` is called.

If `ZLinkActorContext.spotId` is absent, the Actor is a current
[Entry Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
member; if it has a value, it's a member of that User Spot. A separate
boolean or mutable Spot instance representing the same state isn't
provided. `findSpot(actorId)` also only returns the current User
[Spot](../../../../01-glossary.en.md#spot)
[membership](../../../../01-glossary.en.md#membership) as `SpotRef`,
and `undefined` on Entry Spot. The factory creates a new Actor and
context per target attempt, and doesn't reuse an instance whose
cross-node restore failed in the next attempt.

## 3. Session Binding

Session binding fixes the exact incarnation of `ActorRef.actorId +
objectGeneration` once. A bind overload taking a local Actor instance,
a global Actor directory, a handle resolver, and a separate ActorRef
[snapshot](../../../../01-glossary.en.md#snapshot) conversion API aren't
provided. `find(actorId)` only queries an Actor already bound to that
session, not a global directory.

`boundSession`'s push is a one-way operation only sent to the
connection the current binding token specifies. If the connection is
replaced or the binding generation changes, the previous operation
isn't retargeted to the new connection or hidden-retried. Disconnect
only releases the binding — Actor and Spot membership are kept.

The public trace category is `actor-relocation`. The meaning and
verification criteria are owned by
[Actor Model](../../../../14-actor-model.en.md),
[Spot/Actor Membership](../../../../15-spot-actor.en.md), and
[Session Actor Dispatch](../../../../20-session-actor-dispatch.en.md).

`yield(...)` declared on an Actor request is only valid while the
current Actor handler is running on a `SpotWide` User Spot's shared
execution gate. If called by an Entry Spot Actor or a `PerActor` User
Spot's Actor, it completes with `invalidConfiguration`, without
submitting the operation or returning the turn. Actor Join only
provides synchronous `defer()`, and doesn't provide `submit(...)` and
`yield(...)`.
