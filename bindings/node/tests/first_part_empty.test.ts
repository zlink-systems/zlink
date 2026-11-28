// SPDX-License-Identifier: MPL-2.0

import assert from 'node:assert/strict';
import { constants } from 'node:os';
import test from 'node:test';

const { Received, RecvError, RecvResult, TopicMessage } =
  require('@zlink-systems/zlink') as typeof import('../dist/index');

for (const Envelope of [Received, TopicMessage]) {
  test(`${Envelope.name}.firstPart rejects an empty result with InvalidState (EINVAL)`, () => {
    const envelope = new Envelope();
    assert.throws(() => envelope.firstPart(), (error: unknown) => {
      assert.ok(error instanceof RecvError);
      assert.deepEqual(
        [error.result, error.nativeErrno],
        [RecvResult.InvalidState, constants.errno.EINVAL],
      );
      return true;
    });
  });
}
