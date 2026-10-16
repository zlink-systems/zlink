import type { ZLinkSpot, ZLinkSpotContext, ZLinkTimerTick } from '../packages/framework/src';

declare const context: ZLinkSpotContext;

class BoundaryTick {
  async handle(_spot: ZLinkSpot, _tick: ZLinkTimerTick): Promise<void> {}
}

class PacketOnly {
  async handle(_spot: ZLinkSpot, _message: { packet: string }): Promise<void> {}
}

context.handlers.addTimerHandler(BoundaryTick);
// @ts-expect-error A packet-only handler cannot be declared as a timer handler.
context.handlers.addTimerHandler(PacketOnly);
