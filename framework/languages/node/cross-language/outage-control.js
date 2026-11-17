/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

const fs = require('node:fs');
const assert = require('node:assert/strict');
const { ZLinkLocationStoreRepository } = require('../packages/framework/dist/runtime/locations/location-store-repository');
const { encodeAuthorityKey } = require('../packages/framework/dist/runtime/locations/authority-key-codec');
const { opaqueRecordPreimage, routingIdHexSegment } = require('../packages/framework/dist/runtime/locations/opaque-record-key');

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
  constructor(delegate, args, emit, target) {
    this.delegate = delegate;
    this.args = args;
    this.emit = emit;
    this.target = target;
    this.authorityKey = encodeAuthorityKey('actor', args['actor-id']);
    this.actorKey = opaqueRecordPreimage('authority', 'actor', args['actor-id']);
    this.descriptorKey = opaqueRecordPreimage('mesh-node', args['mesh-name'], routingIdHexSegment(args['node-rid']));
    this.observer = new ZLinkLocationStoreRepository(delegate);
    this.holds = new Map();
    this.released = new Set();
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
    await new Promise((resolve, reject) => {
      const aborted = () => {
        this.holds.delete(name);
        reject(signal.reason ?? new Error(`${name} aborted`));
      };
      signal?.throwIfAborted();
      this.holds.set(name, () => {
        signal?.removeEventListener('abort', aborted);
        this.holds.delete(name);
        resolve();
      });
      signal?.addEventListener('abort', aborted, { once: true });
    });
  }

  async write(request, signal) {
    this.failIfOutage();
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
    if (this.recovering) {
      // Temporary #4609 trail: which recovery writes reach the Store and how they end.
      const keys = request.mutations.map(item => `${item.kind}:${item.key.value === this.actorKey ? 'actor'
        : item.key.value === this.descriptorKey ? 'descriptor' : 'other'}`).join(',');
      this.emit(`outage-write|result=${result.kind}|mutations=${keys}`);
    }
    if (result.kind !== 'applied' || !this.recovering) return result;
    const lease = await this.observer.readOwnerLease(this.owner.ownerId);
    if (lease.kind === 'found' && lease.token.leaseGeneration !== this.owner.leaseGeneration) {
      this.newOwner = lease.token;
    }
    if (this.newOwner) {
      if (request.mutations.some(item => item.key.value === this.descriptorKey)) {
        const page = await this.observer.listMeshNodes(this.args['mesh-name']);
        const descriptor = page.items.find(item => String(item.rid) === this.args['node-rid']);
        assert.equal(descriptor?.leaseGeneration, this.newOwner.leaseGeneration);
        this.emit(`outage-descriptor|lease=${descriptor.leaseGeneration}`);
        await this.hold('descriptor', signal);
      }
      if (request.mutations.some(item => item.key.value === this.actorKey)) {
        const authority = await this.observer.readAuthority(this.authorityKey);
        assert.equal(authority.ownerLeaseGeneration, this.newOwner.leaseGeneration);
        assert.equal(authority.objectGeneration, this.baseline.objectGeneration);
        assert.equal(authority.authorityOwnerGeneration, this.baseline.authorityOwnerGeneration);
        this.preserved = authority;
        await this.hold('cas', signal);
      }
    }
    return result;
  }

  async release(name) {
    await until(() => this.holds.has(name), `${name} held`);
    this.released.add(name);
    this.holds.get(name)();
  }

  async commands(handle) {
    let sequence = 0;
    for (;;) {
      const command = await until(() => {
        const file = `${this.args['outage-control-prefix']}.command`;
        if (!fs.existsSync(file)) return undefined;
        const [run, seq, phase] = fs.readFileSync(file, 'utf8').trim().split('\t');
        if (run !== this.args['outage-run-id'] || Number(seq) <= sequence) return undefined;
        assert.equal(Number(seq), sequence + 1, 'outage command sequence gap');
        return { seq: Number(seq), phase };
      }, 'command', null);
      await handle(command.phase);
      sequence = command.seq;
      this.emit(`outage-ack|run=${this.args['outage-run-id']}|seq=${sequence}|phase=${command.phase}`);
      if (command.phase === 'completed') return;
    }
  }

  async dispose() { await this.delegate.dispose(); }
}

function configureOutage(builder) {
  builder.configureLocations().ownerLeaseTtlMs(30000).ownerLeaseRenewIntervalMs(5000)
    .ownerLeaseFencingMarginMs(5000).ownerLeaseRenewTimeoutMs(3000)
    .storeFailureGraceMs(30000).storeOutageRetention(true);
}

module.exports = { OutageStore, until, configureOutage };
