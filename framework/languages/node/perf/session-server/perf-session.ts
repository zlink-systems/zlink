import { Inject, Injectable } from '@nestjs/common';
import { ZLinkPacket } from '@zlink-systems/framework';
import type {
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory,
  ZLinkStreamError
} from '@zlink-systems/framework';
import { PerfEchoRequest } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { PayloadPattern } from '../shared/payload';
import { PerfClock } from '../shared/clock';

// §11.1: STREAM-only receiver; no Object Server, Actor, Store or automatic discovery.
class PerfSession implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly measurement: Measurement
  ) {}

  async onError(_context: ZLinkSessionContext, error: ZLinkStreamError): Promise<void> {
    this.measurement.recordDiagnostic(new Error(`STREAM ${error.error}: ${error.message}`));
  }

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!(await this.context.handlers.tryHandle(dispatch, payload)))
      throw new Error('No typed perf session handler was registered for the packet.');
  }
}

@Injectable()
export class PerfSessionFactory implements ZLinkSessionFactory<PerfSession> {
  constructor(@Inject(Measurement) private readonly measurement: Measurement) {}

  async create(context: ZLinkSessionContext): Promise<PerfSession> {
    context.handlers.addHandler(SessionEchoHandler);
    return new PerfSession(context, this.measurement);
  }
}

@Injectable()
@ZLinkPacket('PerfEchoRequest')
export class SessionEchoHandler {
  constructor(@Inject(Measurement) private readonly measurement: Measurement) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const received = PerfClock.now();
    const measurement = this.measurement;
    measurement.handlerEnter();
    try {
      const request = payload.decode(PerfEchoRequest);
      measurement.validateRequest(request);
      const reply = PayloadPattern.reply(request, received);
      measurement.recordReply(request);
      await context.client.reply(reply).submit();
      if (measurement.phase === 'setup')
        measurement.setupEvidence = [
          {
            kind: 'typedProbeReply',
            source: 'SessionEchoHandler.client.reply.submit',
            observedValue: request.correlationId
          }
        ];
    } catch (error) {
      measurement.recordDiagnostic(error);
      throw error;
    } finally {
      measurement.handlerExit();
    }
  }
}
