const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {
  accessAllowlist,
  compareAccesses,
  scanPrivateAccess
} = require('./fixtures/private-member-access');

const nodeRoot = path.resolve(__dirname, '../..');
const repository = path.resolve(nodeRoot, '../../..');
const allowlistRelative =
  'framework/languages/node/test/contract/fixtures/private-member-access-allowlist.json';

function allowlistGrowth(current, previous) {
  if (Array.isArray(previous)) {
    // Main still has the original file-only baseline; later revisions carry per-access identities.
    const oldFiles = new Set(previous);
    return Object.keys(current)
      .filter((file) => !oldFiles.has(file))
      .map((file) => `Allowlist may only shrink: ${file}`);
  }
  const errors = [];
  for (const [file, entry] of Object.entries(current)) {
    const old = previous[file]?.accesses ?? {};
    for (const [identity, count] of Object.entries(entry.accesses))
      if (count > (old[identity] ?? 0)) errors.push(`Allowlist may only shrink: ${file}`);
  }
  return errors;
}

function readMainAllowlist() {
  execFileSync('git', ['rev-parse', '--verify', 'origin/main^{commit}'], {
    cwd: repository,
    stdio: 'ignore'
  });
  return JSON.parse(
    execFileSync('git', ['show', `origin/main:${allowlistRelative}`], {
      cwd: repository,
      encoding: 'utf8'
    })
  );
}

test('Node tests reject new private accesses and shrink the legacy baseline', () => {
  const probeFile = path.join(__dirname, 'private-access-probe.contract.ts');
  const probe = `
    import { ZLinkFrameworkRuntimeHost } from '../../packages/framework/src/runtime/host';
    import { PrivateHashProbe } from '../../packages/framework/src/runtime/foundation/service-weighted-selection';
    declare const host: ZLinkFrameworkRuntimeHost;
    host.runtimeStatus;
    host['runtimeStatus'];
    (host as any).runtimeStatus;
    const erased = host as any;
    erased.runtimeStatus;
    Reflect.get(host, 'runtimeStatus');
    const copied = erased;
    copied.runtimeStatus;
    const prototypeHost = Object.create(ZLinkFrameworkRuntimeHost.prototype);
    prototypeHost.runtimeStatus;
    declare const mock: any;
    mock.method(host, 'ownerAdmissionOpen');
    const publicObject = { admission: 1 };
    publicObject.admission;
    publicObject['admission'];
    host.status;
    declare const hash: PrivateHashProbe;
    hash.#secret;
    hash['#secret'];
    class PublicHash { ['#secret'] = 1; }
    new PublicHash()['#secret'];
  `;
  const hashSource = path.join(
    nodeRoot,
    'packages/framework/src/runtime/foundation/service-weighted-selection.ts'
  );
  const hashDeclaration =
    fs.readFileSync(hashSource, 'utf8') +
    `
    export class PrivateHashProbe {
      #secret = 1;
      value() { return this.#secret; }
    }
  `;
  const { accesses } = scanPrivateAccess(
    nodeRoot,
    new Map([
      [probeFile, probe],
      [hashSource, hashDeclaration]
    ])
  );
  const probeAccesses = accesses.filter((row) =>
    row.file.endsWith('private-access-probe.contract.ts')
  );
  assert.deepEqual(
    probeAccesses.map((row) => row.member),
    [...Array(7).fill('runtimeStatus'), 'ownerAdmissionOpen', '#secret']
  );
  const actual = accesses.filter((row) => !row.file.endsWith('private-access-probe.contract.ts'));
  const allowed = JSON.parse(
    fs.readFileSync(path.join(__dirname, 'fixtures/private-member-access-allowlist.json'), 'utf8')
  );
  assert.deepEqual(
    compareAccesses(actual, allowed),
    [],
    'Remove or migrate every unlisted private access before updating the baseline'
  );
  assert.deepEqual(allowlistGrowth(allowed, readMainAllowlist()), []);

  const probeBaseline = accessAllowlist(probeAccesses);
  assert.deepEqual(compareAccesses(probeAccesses, probeBaseline), []);
  const withNewAccess = [
    ...probeAccesses,
    probeAccesses.find((row) => row.member === 'runtimeStatus')
  ];
  assert.match(
    compareAccesses(withNewAccess, probeBaseline).join('\n'),
    /Unlisted private access:.*1 accesses/u,
    'A new private access in an already allowed file must fail'
  );
  assert.deepEqual(
    allowlistGrowth(accessAllowlist(withNewAccess), probeBaseline),
    ['Allowlist may only shrink: test/contract/private-access-probe.contract.ts']
  );
});
