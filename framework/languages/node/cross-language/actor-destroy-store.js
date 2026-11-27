/* SPDX-License-Identifier: Elastic-2.0 */

const assert = require('node:assert/strict');
const { AsyncLocalStorage } = require('node:async_hooks');
const {
  ZLinkLocationStoreRepository
} = require('../packages/framework/dist/runtime/locations/location-store-repository');
const {
  encodeAuthorityKey
} = require('../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  decodeActorAuthorityPayload
} = require('../packages/framework/dist/runtime/actors/actor-authority-payload-codec');
const {
  opaqueRecordPreimage,
  routingIdHexSegment
} = require('../packages/framework/dist/runtime/locations/opaque-record-key');
const {
  PROVIDER_STORAGE_NAMESPACE_PREFIX
} = require('../packages/framework/dist/runtime/locations/in-memory-provider-location-store');

const decodeJson = (bytes) => JSON.parse(Buffer.from(bytes).toString('utf8'));

function actorPayload(bytes) {
  const payload = decodeActorAuthorityPayload(bytes);
  assert(payload, 'expected a canonical Actor authority payload');
  return payload;
}

class ActorDestroyStore {
  constructor(delegate, args, owner, emit) {
    this.delegate = delegate;
    this.observer = new ZLinkLocationStoreRepository(delegate);
    this.authorityKey = encodeAuthorityKey('actor', args['actor-id']);
    this.actorKey = opaqueRecordPreimage('authority', 'actor', args['actor-id']);
    this.owner = owner;
    this.scenario = args['actor-destroy-scenario'];
    this.emit = emit;
    this.barriers = new Map();
    this.observations = new Set();
    this.operation = new AsyncLocalStorage();
  }

  async authority() {
    const row = await this.observer.readAuthority(this.authorityKey);
    if (row.kind === 'missing') return { read: 'Missing' };
    const payload = actorPayload(row.payload);
    const [state, operationKind] = { creating: [0, 1], ready: [1, 0], closing: [2, 3] }[
      payload.state
    ];
    return {
      read: 'Found',
      version: row.storeVersion.value,
      generation: row.objectGeneration,
      state,
      operationKind,
      owner: payload.nodeRid,
      ownerId: row.ownerId,
      ownerGeneration: row.authorityOwnerGeneration,
      leaseGeneration: row.ownerLeaseGeneration,
      nodeGeneration: payload.nodeGeneration
    };
  }

  async rememberOwner() {
    if (this.originalOwner) return;
    const authority = await this.authority();
    if (authority.read === 'Found') {
      this.originalGeneration = authority.generation;
      this.originalOwner = {
        ownerId: authority.ownerId,
        leaseGeneration: authority.leaseGeneration
      };
      await this.lease();
    }
  }

  async lease() {
    assert(this.originalOwner, 'original owner token was not observed');
    const { ownerId, leaseGeneration } = this.originalOwner;
    const key = { value: opaqueRecordPreimage('owner-lease', ownerId) };
    const result = await this.delegate.read(key);
    if (result.kind === 'missing') {
      return this.lastExpiry && result.storeNow >= this.lastExpiry ? 'expired' : 'missing';
    }
    const lease = decodeJson(result.value.bytes);
    if (lease.ownerId !== ownerId || BigInt(lease.leaseGeneration) !== leaseGeneration) {
      return 'replaced';
    }
    this.lastExpiry = result.value.expiresAt;
    assert(this.lastExpiry, 'owner lease has no Store expiry');
    return result.value.storeNow < this.lastExpiry ? 'valid' : 'expired';
  }

  async endLease() {
    assert(this.owner && this.originalOwner, 'owner token required to end lease');
    const { ownerId } = this.originalOwner;
    this.endedLeaseKey = opaqueRecordPreimage('owner-lease', ownerId);
    assert.equal(await this.observer.releaseOwnerLease(this.originalOwner), 'released');
    this.emit('ownerLeaseEnded');
  }

  once(event) {
    if (this.observations.has(event)) return;
    this.observations.add(event);
    this.emit(event);
  }

  observeOperation(operation, run) {
    return this.operation.run(operation, run);
  }

  async read(key, signal) {
    const result = await this.delegate.read(key, signal);
    const operation = this.operation.getStore();
    const observesCreate = operation === 'get-or-create';
    const observesDestroy = this.scenario === 'f' && this.originalOwner && operation === 'remote';
    if (
      this.owner ||
      (!observesCreate && !observesDestroy) ||
      key.value !== this.actorKey ||
      result.kind !== 'found'
    )
      return result;

    const row = decodeJson(result.value.bytes);
    const actor = actorPayload(Buffer.from(row.payload, 'base64'));
    if (actor.state === 'closing') {
      if (observesCreate) this.once('createObservedClosing');
      if (
        observesDestroy &&
        BigInt(row.objectGeneration) === this.originalGeneration &&
        (await this.lease()) === 'valid'
      ) {
        this.once('callerObservedClosingWithValidLease');
      }
    }
    return result;
  }

  scan(request, signal) {
    return this.delegate.scan(request, signal);
  }

  dispose() {
    return this.delegate.dispose();
  }

  arm(name) {
    assert(this.owner && !this.barriers.has(name), `cannot arm ${name}`);
    this.barriers.set(name, {});
  }

  held(name) {
    return this.barriers.get(name)?.resolve !== undefined;
  }

  async beforeSubmit(name, event, signal) {
    const barrier = this.barriers.get(name);
    if (!barrier) return;
    assert(!barrier.resolve, `duplicate provider submission for held ${name}`);
    signal?.throwIfAborted();
    const pending = new Promise((resolve, reject) => Object.assign(barrier, { resolve, reject }));
    const aborted = () => barrier.reject(signal.reason);
    signal?.addEventListener('abort', aborted, { once: true });
    try {
      this.emit(event);
      await pending;
    } finally {
      signal?.removeEventListener('abort', aborted);
      this.barriers.delete(name);
    }
    signal?.throwIfAborted();
  }

  release(name, event, error) {
    assert(this.held(name), `${name} provider write is not held`);
    const barrier = this.barriers.get(name);
    this.emit(event);
    if (error) barrier.reject(error);
    else barrier.resolve();
  }

  cancelHolds() {
    for (const barrier of this.barriers.values()) {
      barrier.reject?.(new Error('actor-destroy host finished with a held provider write'));
    }
    this.barriers.clear();
  }

  async write(request, signal) {
    if (
      request.mutations.some((item) => item.kind === 'put' && item.key.value === this.endedLeaseKey)
    ) {
      const lease = await this.delegate.read({ value: this.endedLeaseKey }, signal);
      return {
        kind: 'conflict',
        storeNow: lease.kind === 'found' ? lease.value.storeNow : lease.storeNow
      };
    }
    const mutation = request.mutations.find((item) => item.key.value === this.actorKey);
    if (this.stopping || !mutation) return this.delegate.write(request, signal);

    let committed;
    let fields;
    if (mutation.kind === 'put') {
      const row = decodeJson(mutation.bytes);
      const state = actorPayload(Buffer.from(row.payload, 'base64')).state;
      if (this.owner && state === 'closing') {
        await this.beforeSubmit('cas', 'closingCasHeld', signal);
        committed = 'closingCommitted';
      } else if (!this.owner && state === 'ready' && this.creating) {
        committed = 'newReadyCommitted';
      }
    } else if (this.owner) {
      await this.beforeSubmit('delete', 'ownerDeleteHeld', signal);
      committed = 'ownerDeleteCommitted';
    } else if (this.originalOwner && ['d', 'f'].includes(this.scenario)) {
      fields = await this.releaseFields(request, signal);
      if (fields) {
        if (this.scenario === 'f') this.once('callerObservedClosingForRelease');
        committed = 'releaseCommitted';
      }
    }
    const result = await this.delegate.write(request, signal);
    if (result.kind === 'applied' && committed) this.emit(committed, fields);
    return result;
  }

  async releaseFields(request, signal) {
    const prior = await this.delegate.read({ value: this.actorKey }, signal);
    if (prior.kind !== 'found') return undefined;
    const row = decodeJson(prior.value.bytes);
    const actor = actorPayload(Buffer.from(row.payload, 'base64'));
    if (
      BigInt(row.objectGeneration) !== this.originalGeneration ||
      actor.state !== 'closing' ||
      actor.ownerId !== this.originalOwner.ownerId ||
      actor.ownerLeaseGeneration !== this.originalOwner.leaseGeneration
    )
      return undefined;

    const descriptorKey = opaqueRecordPreimage(
      'mesh-node',
      actor.meshName,
      routingIdHexSegment(actor.nodeRid)
    );
    const capacityKey = `${PROVIDER_STORAGE_NAMESPACE_PREFIX}capacity:${encodeURIComponent(actor.meshName)}:${encodeURIComponent(String(actor.nodeRid))}`;
    const leaseKey = opaqueRecordPreimage('owner-lease', actor.ownerId);
    const condition = (key) => request.conditions.find((item) => item.key.value === key);
    const capacityPut = request.mutations.find(
      (item) => item.key.value === capacityKey && item.kind === 'put'
    );
    const capacity = capacityPut && (await this.delegate.read(capacityPut.key, signal));
    const lease = await this.lease();
    const version = condition(this.actorKey)?.expected?.value;
    return {
      version: version ?? 'missing',
      invalidLease: Boolean(condition(leaseKey)) && lease !== 'valid',
      descriptorCondition: Boolean(condition(descriptorKey)),
      capacityCondition: Boolean(condition(capacityKey)),
      // An Entry membership is represented by the same Actor authority row.
      membershipRemoved: version === prior.value.version.value && actor.currentSpotKind === 'entry',
      capacityDelta:
        capacity?.kind === 'found' &&
        condition(capacityKey)?.expected?.value === capacity.value.version.value
          ? decodeJson(capacityPut.bytes).active.actors -
            decodeJson(capacity.value.bytes).active.actors
          : 'unknown',
      relocationPolicyCondition: request.conditions.some(
        (item) => ![this.actorKey, descriptorKey, capacityKey, leaseKey].includes(item.key.value)
      )
    };
  }
}

module.exports = { ActorDestroyStore };
