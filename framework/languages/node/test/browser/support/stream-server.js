require('reflect-metadata');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkModule, zlinkFramework } = require('@zlink-systems/nestjs');

const endpoint = requiredArgument('--endpoint');
const certificatePath = optionalArgument('--certificate');
const keyPath = optionalArgument('--key');

class EchoPush {
  constructor(value) {
    this.value = value;
  }
}

class BrowserTestSession {
  constructor(context) {
    this.context = context;
  }

  async onDispatch(dispatch, payload) {
    if (dispatch.packetName !== 'EchoReq') {
      throw new Error(`Unsupported browser test packet '${dispatch.packetName}'.`);
    }
    const request = payload.decode(Object);
    process.stdout.write(`${JSON.stringify({ event: 'dispatch', value: request.value })}\n`);
    this.context.client.send(new EchoPush(request.value)).submit();
    this.context.client.reply({ value: request.value }).submit();
    process.stdout.write(`${JSON.stringify({ event: 'replied', value: request.value })}\n`);
  }
}

class BrowserTestSessionFactory {
  async create(context) {
    return new BrowserTestSession(context);
  }
}
Injectable()(BrowserTestSessionFactory);

class BrowserTestModule {}
Module({
  imports: [
    ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = zlinkFramework();
        const stream = builder.addStreamNode('browser-test').bind(endpoint);
        if (certificatePath !== undefined && keyPath !== undefined) {
          stream.setTlsServer(certificatePath, keyPath);
        }
        stream.registerSession(BrowserTestSessionFactory);
        return builder.build();
      }
    })
  ],
  providers: [BrowserTestSessionFactory]
})(BrowserTestModule);

let app;
let stopping = false;
async function main() {
  app = await NestFactory.createApplicationContext(BrowserTestModule, {
    logger: false,
    abortOnError: true
  });
  process.stdout.write(`${JSON.stringify({ event: 'ready', endpoint })}\n`);
  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 25));
  await app.close();
}

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.once(signal, () => { stopping = true; });
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});

function optionalArgument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

function requiredArgument(name) {
  const value = optionalArgument(name);
  if (!value) throw new Error(`${name} is required.`);
  return value;
}
