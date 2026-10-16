import type {
  ZLinkInstanceSpot,
  ZLinkInstanceSpotContext,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkTimerTick
} from '../packages/framework/src';

declare const context: ZLinkSpotContext;
declare const instanceContext: ZLinkInstanceSpotContext;

class BoundaryTick {
  async handle(_spot: ZLinkSpot, _tick: ZLinkTimerTick): Promise<void> {}
}

class InstanceTick {
  async handle(_spot: ZLinkInstanceSpot, _tick: ZLinkTimerTick): Promise<void> {}
}

class PacketOnly {
  async handle(_spot: ZLinkSpot, _message: { packet: string }): Promise<void> {}
}

context.handlers.addTimerHandler(BoundaryTick);
instanceContext.handlers.addTimerHandler(InstanceTick);
// @ts-expect-error A packet-only handler cannot be declared as a timer handler.
context.handlers.addTimerHandler(PacketOnly);
