import { Game, Rejection } from './constants';

export class ChannelRules {
  static validate(channel: bigint): string | null {
    if (channel > Game.maximumInteger || channel < -Game.maximumInteger)
      return Rejection.integerOutOfRange;
    if (channel < 1n || channel > BigInt(Game.channelCount)) return Rejection.invalidChannel;
    return null;
  }
}
