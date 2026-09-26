'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkActivationAdmission } = require('../../packages/framework/dist/internal');

test('a full MeshNode rejects activation admission without waiting for a release', async () => {
  const admission = new ZLinkActivationAdmission(() => 1);
  const release = await admission.acquire('game');

  await assert.rejects(
    admission.acquire('game'),
    /no activation admission headroom/
  );
  assert.deepEqual(admission.current('game'), { active: 1, limit: 1 });

  release();
  const nextRelease = await admission.acquire('game');
  assert.deepEqual(admission.current('game'), { active: 1, limit: 1 });
  nextRelease();
  assert.deepEqual(admission.current('game'), { active: 0, limit: 1 });
});
