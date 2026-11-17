/* SPDX-License-Identifier: Elastic-2.0 */

const fs = require('node:fs');
const assert = require('node:assert/strict');
const path = require('node:path');
const { inspectRuntime } = require('../packages/framework/dist/runtime/internal-observation');
const { ZLinkLocationStoreRepository } = require('../packages/framework/dist/runtime/locations/location-store-repository');
const { encodeAuthorityKey } = require('../packages/framework/dist/runtime/locations/authority-key-codec');
const { opaqueRecordPreimage, routingIdHexSegment } = require('../packages/framework/dist/runtime/locations/opaque-record-key');

function readProfile() {
  const file = process.env.ZLINK_OUTAGE_LEASE_PROFILE
    ?? path.resolve(__dirname, '../../cpp/cross-language/outage-lease-profile.properties');
  const values = Object.fromEntries(fs.readFileSync(file, 'utf8').split(/\r?\n/)
    .map(line => line.trim()).filter(line => line && !line.startsWith('#'))
    .map(line => line.split('=').map(part => part.trim())));
  const profile = {};
  for (const key of ['fencingMarginMs', 'renewIntervalMs', 'ttlMs', 'renewTimeoutMs', 'storeFailureGraceMs']) {
    assert.match(values[key] ?? '', /^\d+$/, `invalid outage profile ${key}`);
    profile[key] = Number(values[key]);
    assert(Number.isSafeInteger(profile[key]) && profile[key] > 0, `invalid outage profile ${key}`);
  }
  assert.equal(values.storeOutageRetention, 'true');
  profile.storeOutageRetention = true;
  return Object.freeze(profile);
}

async function until(observe, label, timeoutMs = 60_000) {
  const deadline = timeoutMs === null ? undefined : Date.now() + timeoutMs;
  for (;;) {
    const value = await observe();
    if (value) return value;
    if (deadline !== undefined && Date.now() >= deadline) throw new Error(`outage phase timed out: ${label}`);
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}

class OutageStore {
  constructor(delegate, args, emit, target, profile) {
    this.delegate = delegate;
    this.args = args;
    this.emit = emit;
    this.target = target;
    this.profile = profile;
    this.authorityKey = encodeAuthorityKey('actor', args['actor-id']);
    this.actorKey = opaqueRecordPreimage('authority', 'actor', args['actor-id']);
    this.descriptorKey = opaqueRecordPreimage('mesh-node', args['mesh-name'], routingIdHexSegment(args['node-rid']));
    this.observer = new ZLinkLocationStoreRepository(delegate);
    this.holds = new Map();
    this.released = new Set();
    this.failure = new Promise(resolve => { this.resolveFailure = resolve; });
  }

  timing(event, monotonicMs = performance.now(), extra = '') {
    this.emit(`outage-timing|event=${event}|monotonicMs=${monotonicMs}${extra}`);
    return monotonicMs;
  }

  fail(error) {
    if (!this.failureError) {
      this.failureError = error;
      this.resolveFailure(error);
    }
    throw this.failureError;
  }

  assertDeadline(event, at = performance.now(), budget = 0) {
    if (this.cutoffMs === undefined || (budget === 0 ? at >= this.cutoffMs : at + budget > this.cutoffMs)) {
      this.fail(new Error(`deadline-exceeded|event=${event}|monotonicMs=${at}|cutoffMs=${this.cutoffMs}|budgetMs=${budget}`));
    }
  }

  async beforeCutoff(pending, event) {
    if (!this.recovering) return pending;
    this.assertDeadline(event);
    let timer;
    const deadline = new Promise((resolve, reject) => {
      timer = setTimeout(() => {
        try {
          this.fail(new Error(`deadline-exceeded|event=${event}|monotonicMs=${performance.now()}|cutoffMs=${this.cutoffMs}`));
        } catch (error) { reject(error); }
      }, Math.ceil(this.cutoffMs - performance.now()));
    });
    try { return await Promise.race([pending, deadline]); }
    finally { clearTimeout(timer); }
  }

  beginOutage(runtime) {
    this.runtime = runtime;
    this.outage = true;
  }

  async captureOutage() {
    const observed = await until(() => {
      const snapshot = this.runtime[inspectRuntime]();
      return snapshot.storeOutage ? snapshot : undefined;
    }, 'runtime outage cutoff');
    this.cutoffMs = observed.storeOutage.cutoffMs;
    assert(Number.isFinite(this.cutoffMs), 'runtime outage cutoff missing');
    this.t0Ms = this.cutoffMs - (this.profile.ttlMs + 2 * this.profile.fencingMarginMs);
    this.timing('t0', this.t0Ms);
    this.timing('cutoff', this.cutoffMs, `|t0Ms=${this.t0Ms}|retentionMs=${this.profile.ttlMs + 2 * this.profile.fencingMarginMs}`);
  }

  failIfOutage() {
    if (!this.outage) return;
    this.failed = true;
    throw new Error('cross-language injected Location Store outage');
  }

  async read(key, signal) {
    this.failIfOutage();
    return this.delegate.read(key, signal);
  }

  async scan(request, signal) {
    this.failIfOutage();
    return this.delegate.scan(request, signal);
  }

  async hold(name, signal) {
    if (this.released.has(name)) return;
    assert(!this.holds.has(name), `duplicate held ${name} operation`);
    await this.beforeCutoff(new Promise((resolve, reject) => {
      const aborted = () => {
        this.holds.delete(name);
        if (this.recovering) {
          try { this.assertDeadline(`${name}-held`); }
          catch (error) { reject(error); return; }
        }
        reject(signal.reason ?? new Error(`${name} aborted`));
      };
      signal?.throwIfAborted();
      this.holds.set(name, () => {
        signal?.removeEventListener('abort', aborted);
        this.holds.delete(name);
        resolve();
      });
      signal?.addEventListener('abort', aborted, { once: true });
    }), `${name}-held`);
  }

  async write(request, signal) {
    const startedAtMs = performance.now();
    this.failIfOutage();
    const actorMutation = request.mutations.find(item => item.key.value === this.actorKey);
    if (this.recovering && actorMutation?.kind === 'delete') {
      this.emit(`discarded|reason=actor-authority-delete|monotonicMs=${startedAtMs}`);
      this.fail(new Error('discarded|reason=actor-authority-delete'));
    }
    if (this.recovering && actorMutation?.kind === 'put') {
      this.timing('cas-submit', startedAtMs);
      this.assertDeadline('cas-submit', startedAtMs, this.profile.renewTimeoutMs);
    }
    // The repository projects the candidate transaction with its own codecs.
    // This also recognizes aggregate commit without duplicating its encoding.
    if (this.target && this.armed && !this.released.has('commit')) {
      const overlay = new ZLinkLocationStoreRepository({
        read: async key => {
          const mutation = request.mutations.find(item => item.key.value === key.value);
          const before = await this.delegate.read(key);
          if (!mutation) return before;
          if (mutation.kind === 'delete') return { kind: 'missing', storeNow: before.storeNow ?? before.value.storeNow };
          return { kind: 'found', value: {
            bytes: mutation.bytes,
            version: before.kind === 'found' ? before.value.version : { value: 'outage-observation' },
            storeNow: before.storeNow ?? before.value.storeNow
          } };
        },
        write: () => { throw new Error('observation repository must not write'); },
        scan: (...values) => this.delegate.scan(...values)
      });
      const candidate = await overlay.readAuthority(this.authorityKey);
      if (candidate.kind === 'snapshot' && candidate.ownerId === this.owner.ownerId) {
        const before = await this.observer.readAuthority(this.authorityKey);
        if (before.kind === 'snapshot' && before.ownerId !== candidate.ownerId) {
          this.stagedAuthority = before;
          await this.hold('commit', signal);
        }
      }
    }
    const result = await this.delegate.write(request, signal);
    if (result.kind !== 'applied' || !this.recovering) return result;
    const lease = await this.observer.readOwnerLease(this.owner.ownerId);
    if (!this.newOwner && lease.kind === 'found' && lease.token.leaseGeneration !== this.owner.leaseGeneration) {
      this.newOwner = lease.token;
      this.timing('new-claim');
    }
    if (this.newOwner) {
      if (!this.descriptorConfirmed && request.mutations.some(item => item.key.value === this.descriptorKey)) {
        const page = await this.observer.listMeshNodes(this.args['mesh-name']);
        const descriptor = page.items.find(item => String(item.rid) === this.args['node-rid']);
        assert.equal(descriptor?.leaseGeneration, this.newOwner.leaseGeneration);
        this.emit(`outage-descriptor|lease=${descriptor.leaseGeneration}`);
        await this.hold('descriptor', signal);
        this.descriptorConfirmed = this.timing('descriptor-confirmed');
        this.assertDeadline('descriptor-confirmed', this.descriptorConfirmed);
      }
      if (actorMutation?.kind === 'put') {
        const authority = await this.observer.readAuthority(this.authorityKey);
        assert.equal(authority.ownerLeaseGeneration, this.newOwner.leaseGeneration);
        assert.equal(authority.objectGeneration, this.baseline.objectGeneration);
        assert.equal(authority.authorityOwnerGeneration, this.baseline.authorityOwnerGeneration);
        this.preserved = authority;
        await this.hold('cas', signal);
        this.casConfirmed = this.timing('cas-confirmed');
        this.assertDeadline('cas-confirmed', this.casConfirmed);
      }
    }
    return result;
  }

  async release(name) {
    await until(() => {
      if (this.recovering) this.assertDeadline(`${name}-held`);
      return this.holds.has(name);
    }, `${name} held`);
    this.released.add(name);
    this.holds.get(name)();
  }

  async commands(handle) {
    let sequence = 0;
    for (;;) {
      const command = await this.beforeCutoff(Promise.race([until(() => {
        const file = `${this.args['outage-control-prefix']}.command`;
        if (!fs.existsSync(file)) return undefined;
        const [run, seq, phase] = fs.readFileSync(file, 'utf8').trim().split('\t');
        if (run !== this.args['outage-run-id'] || Number(seq) <= sequence) return undefined;
        assert.equal(Number(seq), sequence + 1, 'outage command sequence gap');
        return { seq: Number(seq), phase };
      }, 'command', null), this.failure.then(error => { throw error; })]), 'waiting-command').catch(error => {
        this.emit(`outage-error|run=${this.args['outage-run-id']}|seq=${sequence + 1}|phase=waiting-command|error=${String(error?.message ?? error)}`);
        throw error;
      });
      try {
        await this.beforeCutoff(Promise.race([handle(command.phase), this.failure.then(error => { throw error; })]), command.phase);
      } catch (error) {
        this.emit(`outage-error|run=${this.args['outage-run-id']}|seq=${command.seq}|phase=${command.phase}|error=${String(error?.message ?? error)}`);
        throw error;
      }
      sequence = command.seq;
      this.emit(`outage-ack|run=${this.args['outage-run-id']}|seq=${sequence}|phase=${command.phase}`);
      if (command.phase === 'completed') return;
    }
  }

  async dispose() { await this.delegate.dispose(); }
}

function configureOutage(builder) {
  const profile = readProfile();
  builder.configureLocations().ownerLeaseTtlMs(profile.ttlMs).ownerLeaseRenewIntervalMs(profile.renewIntervalMs)
    .ownerLeaseFencingMarginMs(profile.fencingMarginMs).ownerLeaseRenewTimeoutMs(profile.renewTimeoutMs)
    .storeFailureGraceMs(profile.storeFailureGraceMs).storeOutageRetention(profile.storeOutageRetention);
  return profile;
}

module.exports = { OutageStore, until, configureOutage };
