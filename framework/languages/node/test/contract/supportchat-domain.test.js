const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const domainRoot = path.join(
  nodeRoot,
  'samples/SupportChat.Ts/Server/Support/Domain/SupportChat'
);

test('SupportChat agent rejoin preserves WaitingForClose and its deadline', () => {
  const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-supportchat-domain-'));
  try {
    compileDomain(outputRoot);
    const { Conversation } = require(path.join(
      outputRoot,
      'Server/Support/Domain/SupportChat/conversation.js'
    ));
    const conversation = new Conversation(
      'conversation-1',
      'customer-1',
      'Customer',
      'Payment failed',
      1000
    );

    conversation.assign('agent-1', 'Agent', 1100);
    conversation.join('agent-1', 'Agent', 'Agent');
    const deadline = 5000;
    conversation.markIdle(deadline);

    const rejoined = conversation.join('agent-1', 'Agent', 'Agent').state;

    assert.equal(rejoined.status, 'WaitingForClose');
    assert.equal(rejoined.idleDeadlineUnixMs, deadline);

    const resumed = conversation.appendMessage('agent-1', 'I am still here').state;
    assert.equal(resumed.status, 'Active');
    assert.equal(resumed.idleDeadlineUnixMs, undefined);
  } finally {
    fs.rmSync(outputRoot, { recursive: true, force: true });
  }
});

function compileDomain(outputRoot) {
  const result = childProcess.spawnSync(
    process.execPath,
    [
      path.join(nodeRoot, 'node_modules/typescript/bin/tsc'),
      '--target',
      'ES2022',
      '--module',
      'commonjs',
      '--moduleResolution',
      'node',
      '--outDir',
      outputRoot,
      path.join(domainRoot, 'conversation.ts'),
      path.join(domainRoot, 'conversation-events.ts'),
      path.join(domainRoot, 'conversation-models.ts'),
      path.join(domainRoot, 'conversation-policy.ts')
    ],
    { cwd: nodeRoot, encoding: 'utf8' }
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
}
