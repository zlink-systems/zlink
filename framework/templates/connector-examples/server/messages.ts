import { ZLinkPacket } from '@zlink-systems/framework';

@ZLinkPacket('Echo')
export class Echo {
  constructor(
    readonly score: bigint,
    readonly nickname: string | null,
    readonly label?: string
  ) {}
}

@ZLinkPacket('Echoed')
export class Echoed {
  constructor(
    readonly score: bigint,
    readonly nickname: string | null,
    readonly label: string
  ) {}
}

@ZLinkPacket('Notice')
export class Notice {
  constructor(readonly text: string) {}
}

@ZLinkPacket('Left')
export class Left {
  constructor(readonly accepted: boolean) {}
}
