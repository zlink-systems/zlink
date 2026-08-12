import { ZLinkPacket } from '@zlink-systems/framework';

export const RegistrationCodecNames = {
  channel: 'reg-codec'
} as const;

export const PacketNames = {
  echoAutoReq: 'EchoAutoReq',
  echoAutoMsg: 'EchoAutoMsg',
  echoAttrReq: 'EchoAttrReq',
  echoAttrMsg: 'EchoAttrMsg',
  echoManualReq: 'EchoManualReq',
  echoManualMsg: 'EchoManualMsg',
  echoDiReq: 'EchoDiReq',
  echoJsonReq: 'EchoJsonReq',
  echoJsonMsg: 'EchoJsonMsg',
  echoProtobufReq: 'EchoProtobufReq',
  echoProtobufMsg: 'EchoProtobufMsg',
  echoMessagePackReq: 'EchoMessagePackReq',
  echoMessagePackMsg: 'EchoMessagePackMsg'
} as const;

export interface EchoReq {
  readonly value: string;
}

export interface EchoRes {
  readonly value: string;
  readonly contentType: string;
}

export interface EchoMsg {
  readonly commandId: string;
  readonly value: string;
}

export class EchoAutoReq { constructor(readonly value: string) {} }
export class EchoAutoMsg { constructor(readonly commandId: string, readonly value: string) {} }
export class EchoAttrReq { constructor(readonly value: string) {} }
export class EchoAttrMsg { constructor(readonly commandId: string, readonly value: string) {} }
export class EchoManualReq { constructor(readonly value: string) {} }
export class EchoManualMsg { constructor(readonly commandId: string, readonly value: string) {} }
export class EchoDiReq { constructor(readonly value: string) {} }
export class EchoJsonReq { constructor(readonly value: string) {} }
export class EchoJsonMsg { constructor(readonly commandId: string, readonly value: string) {} }

@ZLinkPacket(PacketNames.echoProtobufReq)
export class ProtobufEchoReq {
  constructor(readonly value: string) {}
}

export class ProtobufEchoRes {
  constructor(readonly value: string) {}
}

@ZLinkPacket(PacketNames.echoProtobufMsg)
export class ProtobufEchoMsg {
  constructor(readonly value: string) {}
}

@ZLinkPacket(PacketNames.echoMessagePackReq)
export class MessagePackEchoReq {
  constructor(readonly value: string) {}
}

export class MessagePackEchoRes {
  constructor(readonly value: string) {}
}

@ZLinkPacket(PacketNames.echoMessagePackMsg)
export class MessagePackEchoMsg {
  constructor(
    readonly commandId: string,
    readonly value: string
  ) {}
}

export interface CodecScenarioRes {
  readonly value?: string;
  readonly contentType?: string;
  readonly json?: EchoRes;
  readonly protobufValue?: string;
  readonly messagePackValue?: string;
}

export interface EvidenceWaitReq {
  readonly containsAll: readonly string[];
  readonly timeoutMilliseconds?: number;
}
