/* SPDX-License-Identifier: Elastic-2.0 */
const assert = require('node:assert/strict');
const { on, once } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');

const packet = process.env.ZLINK_LOGIN_QUEUE_PACKET;
const timeoutMs = 30_000;
const marker = (name) => path.join(process.env.ZLINK_LOGIN_QUEUE_DIR, name);
const append = (name, value) => fs.appendFileSync(marker(name), value);

async function waitFor(name) {
  const deadline = performance.now() + timeoutMs;
  while (!fs.existsSync(marker(name))) {
    assert.ok(performance.now() < deadline, `Timed out waiting for ${name}`);
    await delay(10);
  }
}

async function serve({
  settings,
  nestjs,
  NestFactory,
  Module,
  locations,
  writeReady,
  waitForServing
}) {
  const role = process.env.ZLINK_LOGIN_QUEUE_ROLE;
  class SessionFactory {
    create(context) {
      return {
        context,
        async onDispatch(dispatch, payload) {
          assert.equal(dispatch.packetName, packet);
          const value = payload.decode();
          append('handlers.events', `enter|${role}|${value}\n`);
          try {
            if (value === 'held') {
              append('held', '');
              await waitFor('release');
            }
            await context.client.reply(value).submit();
          } finally {
            append('handlers.events', `leave|${role}|${value}\n`);
          }
        }
      };
    }
  }

  const url = `redis://${settings['redis-endpoint']}`;
  const prefix = settings['redis-key-prefix'];
  const builder = nestjs.zlinkFramework();
  builder.addLocationStore(
    new locations.ZLinkRedisLocationStore({ url, keyPrefix: `${prefix}:location` })
  );
  builder.addRelocationStore(
    new locations.ZLinkRedisRelocationStore({ url, keyPrefix: `${prefix}:relocation` })
  );
  builder.configureDispatch().messageFlow('normal');
  const objects = builder
    .addRouteMesh(settings['mesh-name'])
    .listen(settings['bind-endpoint'])
    .setRoutingIdPrefix(role)
    .objects();
  if (role === 'object-server') objects.server();
  else objects.client();
  builder
    .addStreamNode('login')
    .bind(settings['stream-endpoint'])
    .registerSession(SessionFactory)
    .setAuthenticationAdmission(packet, Number(process.env.ZLINK_LOGIN_QUEUE_LOCAL_LIMIT))
    .setAdmissionQueueEnabled(true)
    .setAdmissionQueueKey(process.env.ZLINK_LOGIN_QUEUE_KEY)
    .setAdmissionGlobalLimit(Number(process.env.ZLINK_LOGIN_QUEUE_GLOBAL_LIMIT));
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [SessionFactory]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  app.enableShutdownHooks();
  await waitForServing(app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME));
  writeReady();
}

async function connect(firstEndpoint, secondEndpoint, connector) {
  const clients = [firstEndpoint, secondEndpoint, secondEndpoint].map((endpoint) =>
    connector.zlinkStreamConnectorFactory.create({
      endpoint,
      heartbeat: { enabled: false },
      reconnect: { enabled: false },
      dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
    })
  );
  const [held, second, third] = clients;
  const request = async (client, value) => {
    const reply = await client.request(value).packetName(packet).timeout(timeoutMs).submit();
    assert.equal(reply, value);
    console.log(`admitted ${value}`);
  };
  try {
    for (const client of clients) await client.connect();
    for (const [client, value] of [
      [second, 'second'],
      [third, 'third']
    ]) {
      client.onAdmissionPosition((position, estimatedWaitMs) => {
        assert.ok(position >= 1 && estimatedWaitMs >= 0, `Invalid admission position for ${value}`);
        append(`${value}.position`, `${position}|${estimatedWaitMs}\n`);
      });
    }
    const first = request(held, 'held');
    await Promise.race([
      waitFor('held'),
      first.then(() => assert.fail('Request completed before held marker'))
    ]);
    await Promise.all([first, request(second, 'second'), request(third, 'third')]);
  } finally {
    await Promise.all(clients.map((client) => client.close()));
  }
}

async function probe(endpoint, wire) {
  const signal = AbortSignal.timeout(timeoutMs);
  const sockets = [];
  const kinds = wire.ZlinkStreamMessageKind;
  const json = wire.ZlinkStreamCodec.Json;
  const enqueueName = '$zlink.admission.enqueue';
  const ticketName = '$zlink.admission.ticket';
  const positionName = '$zlink.admission.position';
  async function dial() {
    const socket = new WebSocket(endpoint);
    sockets.push(socket);
    socket.binaryType = 'arraybuffer';
    const messages = on(socket, 'message', { signal, close: ['close'] });
    await once(socket, 'open', { signal });
    return {
      send(name, seq, value, metadata = new Map()) {
        const header = wire.encodeStreamWireHeader({
          kind: kinds.Request,
          codec: json,
          name,
          requestSeq: seq,
          metadata
        });
        socket.send(wire.encodeStreamWireFrame(header, Buffer.from(JSON.stringify(value))));
      },
      async receive() {
        const next = await messages.next();
        assert.ok(!next.done, 'STREAM connection closed before reply');
        const [event] = next.value;
        const frame = wire.decodeStreamWireFrame(new Uint8Array(event.data));
        const header = wire.decodeStreamWireHeader(frame.header);
        assert.equal(header.codec, json);
        return { header, body: JSON.parse(Buffer.from(frame.payload).toString('utf8')) };
      }
    };
  }

  function assertQueueRequired({ header, body }, seq) {
    assert.equal(header.kind, kinds.Error);
    assert.equal(header.name, '');
    assert.equal(header.requestSeq, seq);
    assert.equal(header.metadata.get('$zlink.admission.queueRequired'), '1');
    assert.equal(header.metadata.has('$zlink.admission.retryAfterMs'), false);
    assert.equal(body.code, 'unavailable');
  }

  function assertPosition(body) {
    assert.ok(Number.isInteger(body.position) && body.position >= 1);
    assert.ok(Number.isInteger(body.estimatedWaitMs) && body.estimatedWaitMs >= 0);
  }

  try {
    const owner = await dial();
    const other = await dial();
    owner.send(packet, 1n, 'ticket-owner');
    assertQueueRequired(await owner.receive(), 1n);
    owner.send(enqueueName, 2n, {});
    let ticket;
    let responded = false;
    while (!responded || ticket === undefined) {
      const { header, body } = await owner.receive();
      if (header.kind === kinds.Response) {
        assert.equal(header.name, '');
        assert.equal(header.requestSeq, 2n);
        assert.equal(responded, false, 'Duplicate enqueue response');
        responded = true;
      } else {
        assert.equal(header.kind, kinds.Send);
        assert.ok([ticketName, positionName].includes(header.name));
      }
      if (
        header.name === ticketName ||
        (header.kind === kinds.Response && body.ticket !== undefined)
      ) {
        assert.match(body.ticket, /^[A-Za-z0-9_-]{66}$/);
        assert.ok(Number.isInteger(body.expiresInMs) && body.expiresInMs > 0);
        ticket = body.ticket;
      } else {
        assertPosition(body);
      }
    }

    const metadata = new Map([[ticketName, ticket]]);
    other.send(packet, 1n, 'ticket-replay', metadata);
    assertQueueRequired(await other.receive(), 1n);
    owner.send(packet, 3n, 'ticket-owner', metadata);
    let reply = await owner.receive();
    while (reply.header.kind === kinds.Send && reply.header.name === positionName) {
      assertPosition(reply.body);
      reply = await owner.receive();
    }
    assert.equal(reply.header.kind, kinds.Response);
    assert.equal(reply.header.name, '');
    assert.equal(reply.header.requestSeq, 3n);
    assert.equal(reply.body, 'ticket-owner');
    console.log('Ticket rejected on the other connection and accepted on its issuing connection');
  } finally {
    for (const socket of sockets) socket.close();
  }
}

module.exports = { serve, connect, probe };
