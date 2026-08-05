/* SPDX-License-Identifier: FSL-1.1-ALv2 */

/* Node peer host for the C++ cross-language smoke (G6).
 *
 * Same role modes as the C++ host and the .NET TestHost, built on the public
 * Node packages, so each C++↔Node direction runs over the real wire. Writes a
 * ready file once the role is up and appends markers to the event file. */

const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
/* The Node packages and their peer dependencies resolve from the Node
 * workspace, not from this directory. */
const nodeRoot = path.resolve(__dirname, '../../node');
const { Injectable, Module } = require(path.join(nodeRoot, 'node_modules/@nestjs/common'));
const { NestFactory } = require(path.join(nodeRoot, 'node_modules/@nestjs/core'));
const nestjs = require(path.join(nodeRoot, 'packages/nestjs/dist'));

const args = parseArgs(process.argv.slice(3));
const mode = process.argv[2];

function parseArgs(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument.startsWith('--') && index + 1 < argv.length) {
      values[argument.slice(2)] = argv[index + 1];
      index += 1;
    }
  }
  return values;
}

function require_(name) {
  const value = args[name];
  if (!value) {
    throw new Error(`--${name} is required`);
  }
  return value;
}

function appendEvent(line) {
  console.log(line);
  if (args['event-file']) {
    fs.appendFileSync(args['event-file'], `${line}\n`);
  }
}

function writeReady() {
  if (args['ready-file']) {
    fs.writeFileSync(args['ready-file'], 'ready\n');
  }
}

class TestHostProfileRequest {
  constructor(value) { this.value = value; }
}
class TestHostProfileSend {
  constructor(value) { this.value = value; }
}
class TestHostPublishedEvent {
  constructor(value) { this.value = value; }
}

function readValue(payload) {
  return payload?.value ?? payload?.Value;
}

async function channelServer() {
  class ProfileRequestHandler {
    async handle(payload) {
      appendEvent(`channel-server-request|${readValue(payload)}`);
      return { value: readValue(payload) };
    }
  }
  Injectable()(ProfileRequestHandler);
  class ProfileSendHandler {
    async handle(payload) {
      appendEvent(`channel-server-send|${readValue(payload)}`);
    }
  }
  Injectable()(ProfileSendHandler);

  class ServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addClientServerChannel(require_('channel-name'))
          .enableServer(require_('server-endpoint'))
          .addRequestHandler('TestHostProfileRequest', ProfileRequestHandler)
          .addSendHandler('TestHostProfileSend', ProfileSendHandler);
        return builder.build();
      }
    })],
    providers: [ProfileRequestHandler, ProfileSendHandler]
  })(ServerModule);

  const app = await NestFactory.createApplicationContext(ServerModule, { logger: false });
  writeReady();
  await new Promise(() => {});
  await app.close();
}

async function channelClient() {
  class ClientModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => nestjs.zlinkFramework()
        .addClientServerChannel(require_('channel-name'))
        .enableClient(require_('server-endpoint'))
        .build()
    })]
  })(ClientModule);

  const app = await NestFactory.createApplicationContext(ClientModule, { logger: false });
  const client = app.get(nestjs.ZLINK_CHANNEL_CLIENT, { strict: false });
  const value = args.value ?? 'node-to-cpp';
  const reply = await client
    .requestToChannel(require_('channel-name'), new TestHostProfileRequest(value))
    .timeout(5000)
    .submit();
  appendEvent(`channel-client-reply|${readValue(reply)}`);
  await client
    .sendToChannel(require_('channel-name'), new TestHostProfileSend(`${value}-send`))
    .submit();
  appendEvent(`channel-client-sent|${value}-send`);
  writeReady();
  await new Promise(() => {});
}

async function channelSubscriber() {
  class PublishedEventHandler {
    async handle(payload, context) {
      appendEvent(`${context.topic}:${readValue(payload)}`);
    }
  }
  Injectable()(PublishedEventHandler);

  class SubscriberModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addFanoutChannel(require_('channel-name'))
          .enableSubscriber(require_('publisher-endpoint'))
          .addPublishHandler('TestHostPublishedEvent', PublishedEventHandler);
        return builder.build();
      }
    })],
    providers: [PublishedEventHandler]
  })(SubscriberModule);

  const app = await NestFactory.createApplicationContext(SubscriberModule, { logger: false });
  writeReady();
  await new Promise(() => {});
  await app.close();
}

async function channelPublisher() {
  class PublisherModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => nestjs.zlinkFramework()
        .addFanoutChannel(require_('channel-name'))
        .enablePublisher(require_('publisher-endpoint'))
        .build()
    })]
  })(PublisherModule);

  const app = await NestFactory.createApplicationContext(PublisherModule, { logger: false });
  const publisher = app.get(nestjs.ZLINK_FANOUT_CLIENT, { strict: false });
  const topic = require_('topic');
  const value = args.value ?? 'node-publish';
  writeReady();
  /* A subscriber may still be connecting: repeat like the peer publishers do. */
  for (;;) {
    await publisher.publish(require_('channel-name'), topic, new TestHostPublishedEvent(value)).submit();
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
}

async function streamConnector() {
  const driverModule = await import(pathToFileURL(
    path.join(nodeRoot, 'scripts/browser-e2e/connector-driver.mjs')
  ).href);
  const instance = await driverModule.createBrowserConnectorDriver();
  await instance.connect(require_('stream-endpoint'));
  const value = args.value ?? 'node-connector-to-cpp';
  const reply = await instance.request('RawPing', value, true);
  appendEvent(`connector-reply|${typeof reply === 'string' ? reply : JSON.stringify(reply)}`);
  writeReady();
  await instance.close();
}

function messageFollowRoute(
  targetNodeRid,
  actorNodeRid,
  targetNodeGeneration,
  authorityOwnerGeneration
) {
  return {
    kind: 'actor',
    actor: {
      nodeRid: actorNodeRid,
      actorId: 'cross-language-message-follow',
      generation: 1n
    },
    targetNodeRid,
    targetNodeGeneration,
    authorityOwnerGeneration,
    ownerLeaseGeneration: 8n
  };
}

async function messageFollow() {
  const { ZLinkNodeRawMeshBackend } = require(path.join(
    nodeRoot,
    'packages/framework/dist/runtime/backend/node/node-raw-mesh-backend.js'
  ));
  const localRid = require_('node-rid');
  const peerRid = require_('peer-rid');
  const backend = new ZLinkNodeRawMeshBackend('message-follow', localRid);
  let sent = false;
  let received = false;
  let finished = false;
  backend.setBind(require_('bind-endpoint'));
  backend.setMessageFollowHandler((record) => {
    appendEvent(
      `message-follow-received|source-node=${record.source.targetNodeRid}`
      + `|target-node=${record.target.targetNodeRid}`
      + `|operation-low=${record.originalOperation.low.toString()}`
    );
    received = true;
  });
  backend.start();
  backend.connectPeer({
    endpoint: require_('peer-endpoint'),
    expectedRid: peerRid,
    expectedLifecycleGeneration: 1n
  });
  writeReady();

  const finish = (code, error) => {
    if (finished) return;
    finished = true;
    if (error) console.error(`node message-follow failed: ${error}`);
    backend.close();
    process.exitCode = code;
  };
  const deadline = Date.now() + 60_000;
  const timer = setInterval(() => {
    try {
      const peer = backend.peers().find(
        (entry) => String(entry.routingId) === peerRid
      );
      const routeReady = peer !== undefined
        && backend.isPeerRouteReady(peerRid, peer.lifecycleGeneration);
      if (!sent && routeReady) {
        const local = backend.status();
        backend.sendMessageFollowNotification(peerRid, {
          source: messageFollowRoute(
            localRid,
            localRid,
            local.lifecycleGeneration,
            7n
          ),
          target: messageFollowRoute(
            peerRid,
            localRid,
            peer.lifecycleGeneration,
            8n
          ),
          hopCount: 1,
          queuedMessages: 1,
          queuedBytes: 64,
          originalOperation: { high: 3n, low: 202n },
          originalReplyRouteId: 11n
        });
        appendEvent('message-follow-sent|operation-low=202');
        sent = true;
      }
      if (sent && received) {
        clearInterval(timer);
        finish(0);
      } else if (Date.now() >= deadline) {
        clearInterval(timer);
        finish(1, new Error(`exchange timed out (sent=${sent}, received=${received})`));
      }
    } catch (error) {
      clearInterval(timer);
      finish(1, error);
    }
  }, 2);
}

async function main() {
  switch (mode) {
    case 'channel-server': return channelServer();
    case 'channel-client': return channelClient();
    case 'channel-subscriber': return channelSubscriber();
    case 'channel-publisher': return channelPublisher();
    case 'browser-stream-connector': return streamConnector();
    case 'message-follow': return messageFollow();
    default: throw new Error(`unsupported mode '${mode}'`);
  }
}

main().catch((error) => {
  console.error(`node peer host failed: ${error?.stack ?? error}`);
  process.exit(1);
});
