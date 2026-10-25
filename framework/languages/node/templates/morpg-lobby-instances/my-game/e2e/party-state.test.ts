import assert from 'node:assert/strict';
import { test } from 'node:test';
import { PartyState } from '../server/lobby/domain';
import { PartyJoin } from '../server/shared/messages';
import { Game, Rejection } from '../server/shared/constants';

test('Party admission preserves leader, capacity, and readiness', () => {
  const party = new PartyState();
  assert.equal(party.join('leader', new PartyJoin('party', false)), false);
  assert.equal(party.join('leader', new PartyJoin(' ', true)), false);
  assert.equal(party.join('leader', new PartyJoin('party', true)), true);
  assert.equal(party.join('leader', new PartyJoin('party', false)), false);
  assert.equal(party.join('other', new PartyJoin('different', false)), false);
  assert.equal(party.join('other', new PartyJoin('party', true)), false);
  for (let index = 1; index < Game.maximumPartyMembers; index++)
    assert.equal(party.join(`member-${index}`, new PartyJoin('party', false)), true);
  assert.equal(party.join('overflow', new PartyJoin('party', false)), false);
  assert.equal(party.entryRejection('member-1'), Rejection.notLeader);
  assert.equal(party.entryRejection('leader'), Rejection.notReady);
  for (const id of party.members) {
    party.markReady(id);
    party.markReady(id);
  }
  assert.equal(party.ready.length, Game.maximumPartyMembers);
  assert.equal(party.entryRejection('leader'), null);
  const restored = Object.assign(new PartyState(), JSON.parse(JSON.stringify(party)));
  assert.deepEqual(restored.view(), party.view());
  party.leave('leader');
  assert.equal(party.entryRejection('member-1'), null);
  assert.equal(party.ready.includes('leader'), false);
  assert.equal(party.join('replacement', new PartyJoin('party', false)), true);
  assert.equal(party.entryRejection('member-1'), Rejection.notReady);
});

import { domainChecks } from './domain-checks';
test('Run state boundaries', domainChecks);
