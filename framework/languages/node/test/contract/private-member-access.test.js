const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { scanPrivateAccess } = require('./fixtures/private-member-access');

const nodeRoot = path.resolve(__dirname, '../..');

test('tests use public operations or internal constructor seams instead of private members', () => {
  const probeFile = path.join(__dirname, 'private-access-probe.contract.ts');
  const probe = `
    import { ZLinkFrameworkRuntimeHost } from '../../packages/framework/src/runtime/host';
    import { PrivateHashProbe } from '../../packages/framework/src/runtime/foundation/service-weighted-selection';
    declare const host: ZLinkFrameworkRuntimeHost;
    host.admission;
    host['admission'];
    (host as any).admission;
    const erased = host as any;
    erased.admission;
    Reflect.get(host, 'admission');
    const copied = erased;
    copied.admission;
    const prototypeHost = Object.create(ZLinkFrameworkRuntimeHost.prototype);
    prototypeHost.admission;
    declare const mock: any;
    mock.method(host, 'preflightAutomaticPeerReadiness');
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
    [...Array(7).fill('admission'), 'preflightAutomaticPeerReadiness', '#secret']
  );
  const actual = accesses.filter((row) => !row.file.endsWith('private-access-probe.contract.ts'));
  const allowed = JSON.parse(
    fs.readFileSync(path.join(__dirname, 'fixtures/private-member-access-allowlist.json'), 'utf8')
  );
  const actualFiles = [...new Set(actual.map((row) => row.file))].sort();
  const unexpected = actual.filter((row) => !allowed.includes(row.file));
  assert.deepEqual(
    unexpected,
    [],
    unexpected.map((row) => `${row.file}:${row.line}: private ${row.member}`).join('\n')
  );
  // A file leaves the baseline as soon as its last access is migrated.
  assert.deepEqual(actualFiles, allowed, 'Remove stale files from the private access allowlist');
});
