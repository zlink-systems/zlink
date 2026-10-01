import { PerfClock } from './clock';
import { DecimalText, PerfEchoReply, PerfEchoRequest, PerfValidationException } from './contracts';

// §15.2: logical byte b[i] = (31*i + 17*floor(i/251) + 29) mod 256, carried as canonical padded Base64.
export class PayloadPattern {
  readonly base64: string;
  constructor(readonly size: number) {
    const bytes = Buffer.alloc(size);
    for (let i = 0; i < size; i++) bytes[i] = (31 * i + 17 * Math.floor(i / 251) + 29) % 256;
    this.base64 = bytes.toString('base64');
  }

  // The canonical Base64 of the pattern is unique, so text equality proves the length and every logical byte.
  validate(payload: unknown): void {
    if (payload !== this.base64) throw new PerfValidationException('PayloadMismatch', 'Payload is not the canonical Base64 pattern.');
  }

  static validateIdentity(request: PerfEchoRequest, reply: PerfEchoReply): void {
    if (
      request.runId !== reply.runId || request.cellId !== reply.cellId || request.resetSeq !== reply.resetSeq ||
      request.phase !== reply.phase || request.clientId !== reply.clientId || request.sequence !== reply.sequence ||
      request.correlationId !== reply.correlationId || !reply.clockDomainId
    ) throw new PerfValidationException('IdentityMismatch', 'Echo identity differs from the submitted operation.');
    DecimalText.i64(reply.receivedTicks);
  }

  static reply(request: PerfEchoRequest, receivedTicks: bigint): PerfEchoReply {
    return new PerfEchoReply({
      runId: request.runId, cellId: request.cellId, resetSeq: request.resetSeq, phase: request.phase,
      clientId: request.clientId, sequence: request.sequence, correlationId: request.correlationId,
      receivedTicks: receivedTicks.toString(), clockDomainId: PerfClock.domain, payload: request.payload
    });
  }
}
