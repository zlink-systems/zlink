import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { encodeBoundSessionCloseCommand } from '../../packages/framework/src/runtime/protocol/service_wire_codec.generated';
import { sessionOwnerFenceMatches } from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import {
  decodeStatefulHeader,
  encodeBoundSessionReplacedHeader
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

/**
 * Session-Actor binding §4: every runtime encodes the stored Session owner
 * identity unchanged in commands 51 and 54, and the receiving Session owner
 * accepts only an exact match with its current values. The same bytes are
 * checked by the .NET, Java and C++ suites.
 */
interface SessionOwnerIdentityFixture {
  readonly sessionOwner: {
    readonly nodeRidHex: string;
    readonly nodeGeneration: string;
    readonly ownerId: string;
    readonly leaseGeneration: string;
  };
  readonly session: { readonly sessionRidHex: string; readonly bindingGeneration: string };
  readonly actor: {
    readonly actorId: string;
    readonly objectGeneration: string;
    readonly ownerNodeRidHex: string;
    readonly ownerNodeGeneration: string;
    readonly authorityOwnerGeneration: string;
    readonly ownerLeaseGeneration: string;
  };
  readonly correlation: string;
  readonly accepted: { readonly command51Hex: string; readonly command54Hex: string };
  readonly rejected: readonly {
    readonly name: string;
    readonly command51Hex: string;
    readonly command54Hex: string;
  }[];
}

test('Session owner identity contract: commands 51 and 54 carry the descriptor identity and the Session owner compares it exactly', () => {
  const fixture = JSON.parse(
    readFileSync('../../runtime/protocol/fixtures/session-owner-identity-v1.json', 'utf8')
  ) as SessionOwnerIdentityFixture;
  const text = (hex: string): string => Buffer.from(hex, 'hex').toString('utf8');
  const local = {
    nodeRid: text(fixture.sessionOwner.nodeRidHex),
    nodeGeneration: BigInt(fixture.sessionOwner.nodeGeneration),
    ownerId: fixture.sessionOwner.ownerId,
    leaseGeneration: BigInt(fixture.sessionOwner.leaseGeneration)
  };
  const retiredSession = {
    sessionOwnerNodeRid: local.nodeRid,
    sessionOwnerNodeGeneration: local.nodeGeneration,
    sessionOwnerId: local.ownerId,
    sessionOwnerLeaseGeneration: local.leaseGeneration,
    sessionRid: text(fixture.session.sessionRidHex),
    retiredBindingGeneration: BigInt(fixture.session.bindingGeneration)
  };
  const actorAuthority = {
    actor: {
      nodeRid: text(fixture.actor.ownerNodeRidHex),
      actorId: fixture.actor.actorId,
      generation: BigInt(fixture.actor.objectGeneration)
    },
    targetNodeGeneration: BigInt(fixture.actor.ownerNodeGeneration),
    authorityOwnerGeneration: BigInt(fixture.actor.authorityOwnerGeneration),
    ownerLeaseGeneration: BigInt(fixture.actor.ownerLeaseGeneration)
  };

  // The Actor owner encodes the stored identity unchanged.
  assert.equal(
    encodeBoundSessionReplacedHeader(actorAuthority, retiredSession).toString('hex'),
    fixture.accepted.command51Hex
  );
  assert.equal(
    Buffer.from(
      encodeBoundSessionCloseCommand(
        {
          command: 'boundSessionClose',
          flags: 0,
          correlation: BigInt(fixture.correlation),
          actor: {
            actorId: actorAuthority.actor.actorId,
            objectGeneration: actorAuthority.actor.generation
          },
          session: {
            sessionOwnerNodeRid: Buffer.from(fixture.sessionOwner.nodeRidHex, 'hex'),
            sessionOwnerNodeGeneration: retiredSession.sessionOwnerNodeGeneration,
            sessionOwnerId: retiredSession.sessionOwnerId,
            sessionOwnerLeaseGeneration: retiredSession.sessionOwnerLeaseGeneration,
            sessionRid: Buffer.from(fixture.session.sessionRidHex, 'hex'),
            retiredBindingGeneration: retiredSession.retiredBindingGeneration
          }
        },
        {}
      )[0]!
    ).toString('hex'),
    fixture.accepted.command54Hex
  );

  // The Session owner accepts only an exact match with its current values.
  const fenceOf = (hex: string) => {
    const decoded = decodeStatefulHeader(Buffer.from(hex, 'hex'));
    if (decoded.kind === 'boundSessionReplaced') return decoded.retiredSession;
    if (decoded.kind === 'boundSessionClose') return decoded.session;
    return assert.fail(`unexpected command ${decoded.kind}`);
  };
  assert.equal(sessionOwnerFenceMatches(local, fenceOf(fixture.accepted.command51Hex)), true);
  assert.equal(sessionOwnerFenceMatches(local, fenceOf(fixture.accepted.command54Hex)), true);
  for (const rejected of fixture.rejected) {
    assert.equal(
      sessionOwnerFenceMatches(local, fenceOf(rejected.command51Hex)),
      false,
      rejected.name
    );
    assert.equal(
      sessionOwnerFenceMatches(local, fenceOf(rejected.command54Hex)),
      false,
      rejected.name
    );
  }
});
