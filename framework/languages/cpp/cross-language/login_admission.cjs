/* SPDX-License-Identifier: Elastic-2.0 */
const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');

const env = process.env;
const marker = (name) => path.join(env.ZLINK_LOGIN_DIR, name);

async function waitFor(name) {
  const deadline = performance.now() + Number(env.ZLINK_LOGIN_TIMEOUT_MS);
  while (!fs.existsSync(marker(name))) {
    assert.ok(performance.now() < deadline, `Timed out waiting for ${name}`);
    await delay(5);
  }
}

async function serve({ endpoint, nestjs, NestFactory, Module, writeReady, waitForServing }) {
  class SessionFactory {
    create(context) {
      return {
        context,
        async onDispatch(_dispatch, payload) {
          const value = payload.decode();
          fs.appendFileSync(marker('server.events'), `${value}|${context.sessionId}\n`);
          if (value === env.ZLINK_LOGIN_HELD) {
            fs.writeFileSync(marker('held'), '');
            await waitFor('release');
          }
          await context.client.reply(value).submit();
        }
      };
    }
  }

  const builder = nestjs.zlinkFramework();
  builder
    .addStreamNode('stream.login-admission')
    .bind(endpoint)
    .registerSession(SessionFactory)
    .setAuthenticationAdmission(
      env.ZLINK_LOGIN_PACKET,
      Number(env.ZLINK_LOGIN_LIMIT),
      Number(env.ZLINK_LOGIN_RETRY_MS)
    );
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

async function connect(endpoint, connector) {
  const clients = [];
  try {
    for (let index = 0; index < 2; ++index) {
      const client = connector.zlinkStreamConnectorFactory.create({
        endpoint,
        heartbeat: { enabled: false },
        reconnect: { enabled: false },
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
      });
      clients.push(client);
      await client.connect();
    }
    const [held, contender] = clients;
    contender.onAdmissionWaiting((event) => {
      fs.appendFileSync(marker('waiting'), `${event.requestPacketName}|${event.retryAfterMs}\n`);
    });
    const request = async (client, value) => {
      const reply = await client
        .request(value)
        .packetName(env.ZLINK_LOGIN_PACKET)
        .timeout(Number(env.ZLINK_LOGIN_TIMEOUT_MS))
        .submit();
      assert.equal(reply, value);
    };
    await request(contender, env.ZLINK_LOGIN_CONTROL);
    await Promise.all([
      request(held, env.ZLINK_LOGIN_HELD),
      waitFor('held').then(() => request(contender, env.ZLINK_LOGIN_CONTENDER))
    ]);
  } finally {
    await Promise.all(clients.map((client) => client.close()));
  }
}

async function probe(endpoint, wire) {
  const fixture = require(env.ZLINK_LOGIN_FIXTURE);
  const scenario = fixture.serverScenarios.find(
    ({ name }) => name === fixture.crossLanguage.serverScenario
  );
  const expected = scenario.steps.at(-1).expect;
  const sockets = [];
  const signal = AbortSignal.timeout(fixture.crossLanguage.timeoutMs);
  async function dial() {
    const socket = new WebSocket(endpoint);
    socket.binaryType = 'arraybuffer';
    sockets.push(socket);
    await once(socket, 'open', { signal });
    return socket;
  }
  const send = (socket, value, sequence) => {
    const reply = once(socket, 'message', { signal }).then(([event]) => {
      const frame = wire.decodeStreamWireFrame(new Uint8Array(event.data));
      return {
        header: wire.decodeStreamWireHeader(frame.header),
        body: JSON.parse(Buffer.from(frame.payload).toString('utf8'))
      };
    });
    const header = wire.encodeStreamWireHeader({
      kind: wire.ZlinkStreamMessageKind.Request,
      codec: wire.ZlinkStreamCodec.Json,
      name: scenario.given.authenticationPacketName,
      requestSeq: BigInt(sequence),
      metadata: new Map()
    });
    socket.send(wire.encodeStreamWireFrame(header, Buffer.from(JSON.stringify(value))));
    return reply;
  };
  try {
    const held = await dial();
    const contender = await dial();
    const pending = send(held, fixture.crossLanguage.held, 1);
    await Promise.all([
      pending.then((reply) => {
        assert.equal(reply.header.kind, wire.ZlinkStreamMessageKind.Response);
        assert.equal(reply.body, fixture.crossLanguage.held);
      }),
      (async () => {
        await waitFor('held');
        try {
          const { header, body } = await send(
            contender,
            fixture.crossLanguage.contender,
            expected.error.requestSeq
          );
          assert.equal(header.kind, wire.ZlinkStreamMessageKind[expected.error.kind]);
          assert.equal(header.codec, wire.ZlinkStreamCodec.Json);
          assert.equal(header.name, expected.error.packetName);
          assert.equal(String(header.requestSeq), expected.error.requestSeq);
          assert.deepEqual(Object.fromEntries(header.metadata), expected.error.metadata);
          assert.deepEqual(body, expected.error.body);
          assert.equal(contender.readyState === WebSocket.CLOSED, expected.error.connectionClosed);
          const handlers = fs.readFileSync(marker('server.events'), 'utf8').split('\n');
          assert.equal(
            handlers.filter((line) => line.startsWith(`${fixture.crossLanguage.contender}|`))
              .length,
            expected.handlerCallsForRequest
          );
          console.log(
            `wire rejection ${JSON.stringify(body)} ${JSON.stringify(expected.error.metadata)}`
          );
        } finally {
          fs.writeFileSync(marker('release'), '');
        }
      })()
    ]);
    const control = await send(contender, fixture.crossLanguage.control, 3);
    assert.equal(control.header.kind, wire.ZlinkStreamMessageKind.Response);
    assert.equal(control.body, fixture.crossLanguage.control);
  } finally {
    for (const socket of sockets) socket.close();
  }
}

module.exports = { serve, connect, probe };
