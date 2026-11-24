import 'reflect-metadata';
import './.zlink-framework-json-schemas.cjs';
import { appendFileSync } from 'node:fs';
import { once } from 'node:events';
import { logPath, tracing } from './tracing';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkFrameworkTerminationOutcome,
  type ZLinkFrameworkRuntime,
  type ZLinkMessage,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory
} from '@zlink-systems/framework';
import { ZLINK_FRAMEWORK_RUNTIME, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { Echo, Echoed, Left, Notice } from './messages';

const streamName = 'connector-example';
const endpoint = process.env.CONNECTOR_EXAMPLE_BIND ?? 'ws://127.0.0.1:7301';

function log(line: string): void {
  appendFileSync(logPath, line + '\n');
  console.log(line);
}

class ExampleSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const { client } = this.context;
    switch (dispatch.packetName) {
      case 'Publish':
        await client.send(new Notice('Hello from the server')).submit();
        return;
      case 'Echo': {
        const echo = payload.decode(Echo);
        await client.reply(new Echoed(echo.score, echo.nickname, echo.label ?? 'default')).submit();
        return;
      }
      case 'Reconnect':
        // Simulate a dropped connection while keeping the listener available.
        await this.context.close();
        return;
      case 'Leave':
        log('Left ' + this.context.sessionId);
        await client.reply(new Left(true)).submit();
        return;
      default:
        throw new Error('Unrecognized packet: ' + dispatch.packetName);
    }
  }
}

@Injectable()
class ExampleSessionFactory implements ZLinkSessionFactory<ExampleSession> {
  async create(context: ZLinkSessionContext): Promise<ExampleSession> {
    return new ExampleSession(context);
  }
}

const builder = zlinkFramework();
builder.configureDispatch().messageFlow('normal');
builder.addStreamNode(streamName).bind(endpoint).registerSession(ExampleSessionFactory);

@Module({
  imports: [ZLinkModule.forRoot(builder.build())],
  providers: [ExampleSessionFactory]
})
class ExampleServer {}

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ExampleServer, {
    logger: ['error', 'warn'],
    abortOnError: false
  });
  const runtime = app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME);
  const stop = new AbortController();
  const shutdown = (): void => stop.abort();
  process.once('SIGINT', shutdown);
  process.once('SIGTERM', shutdown);
  try {
    let serving = false;
    for await (const observed of runtime.observe(AbortSignal.timeout(15_000))) {
      if (observed.status.state === ZLinkFrameworkRuntimeState.Serving) {
        serving = true;
        break;
      }
      if (observed.status.state !== ZLinkFrameworkRuntimeState.Preparing) {
        throw new Error('Server startup failed: ' + observed.status.state);
      }
    }
    if (!serving) throw new Error('Startup observation ended before Serving');
    const listener = runtime.getListenerStatus('stream', streamName);
    log('READY ' + listener.endpoint);
    log('Log file ' + logPath);
    if (!stop.signal.aborted) await once(stop.signal, 'abort');
  } finally {
    process.off('SIGINT', shutdown);
    process.off('SIGTERM', shutdown);
    try {
      const result = await runtime.shutdown({ deadlineMs: 10_000 });
      if (result.outcome !== ZLinkFrameworkTerminationOutcome.Stopped) {
        throw new Error('Server shutdown failed: ' + result.reason);
      }
    } finally {
      try {
        await app.close();
      } finally {
        await tracing.shutdown();
      }
    }
  }
}

main().catch((error: unknown) => {
  log(error instanceof Error ? (error.stack ?? error.message) : String(error));
  process.exitCode = 1;
});
