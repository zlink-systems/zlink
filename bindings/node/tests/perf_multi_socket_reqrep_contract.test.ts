// SPDX-License-Identifier: MPL-2.0

'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const zlink = require('@zlink-systems/zlink');
const source = fs.readFileSync(
  path.resolve(__dirname, '../../perf/multi/perf_multi_socket_reqrep.ts'),
  'utf8'
);

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

test('multi REQREP keeps one completion poller progressing during active and drain turns', () => {
  assert.match(source, /completionPoller = zlink\.createPoller\(\)/);
  assert.match(
    source,
    /completionPoller\.add\(sockets\[index\], pollEvents\(POLLCOMPLETION\), index\)/
  );
  assert.equal(
    (source.match(/waitPollerOne\(completionPoller, completionEvents, waitMs\)/g) ?? []).length,
    1,
    'the active submit turn must perform one completion poll'
  );
  assert.match(
    source,
    /while \(\(pending\.size > 0 \|\| blocked\.size > 0\)[\s\S]*?waitPollerOne\(/
  );
  assert.doesNotMatch(source, /Promise\.race\(blocked\.values\(\)\)/);
});

test('one completion-only poller settles concurrent requests from multiple sockets', async () => {
  const context = zlink.createContext();
  const server = zlink.createRouterSocket(context);
  const clients = Array.from({ length: 4 }, () => zlink.createDealerSocket(context));
  const completionPoller = zlink.createPoller();
  const completionEvents = zlink.createPollEvents(clients.length);
  const received = new zlink.Received();
  const endpoint = `inproc://node-perf-multi-reqrep-${process.pid}`;

  try {
    server.bind(endpoint);
    clients.forEach((client, index) => {
      client.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${index}`)));
      client.connect(endpoint);
      completionPoller.add(
        client,
        [zlink.PollEventFlag.PollCompletion],
        index
      );
    });

    const expectedReplies = 32;
    let settledReplies = 0;
    const replies: Promise<void>[] = [];
    for (let sequence = 0; sequence < expectedReplies; sequence += 1) {
      const client = clients[sequence % clients.length];
      const expected = `request-${sequence}`;
      const submission = client.request()
        .message(expected)
        .timeout(1_000)
        .submit();
      replies.push(submission.reply.then((parts) => {
        try {
          assert.equal(parts[0].getString(), expected);
          settledReplies += 1;
        } finally {
          parts.forEach((part) => part.close());
        }
      }));
    }

    let serverReplies = 0;
    const deadline = Date.now() + 5_000;
    while (settledReplies < expectedReplies && Date.now() < deadline) {
      while (server.recv(received, zlink.RecvFlags.DontWait)) {
        received.reply().message(received.parts[0]).submit();
        received.close();
        serverReplies += 1;
      }
      completionPoller.wait(completionEvents, 0);
      await nextTurn();
    }

    assert.equal(serverReplies, expectedReplies);
    assert.equal(settledReplies, expectedReplies);
    await Promise.all(replies);
  } finally {
    received.close();
    completionEvents.close();
    completionPoller.close();
    clients.forEach((client) => client.close());
    server.close();
    context.close();
  }
});
