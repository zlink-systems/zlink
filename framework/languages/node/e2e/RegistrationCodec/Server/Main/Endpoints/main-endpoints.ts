import type { ZLinkRouteClient } from '@zlink-systems/framework';
import {
  EchoAttrMsg,
  EchoAttrReq,
  EchoAutoMsg,
  EchoAutoReq,
  EchoDiReq,
  EchoJsonMsg,
  EchoJsonReq,
  EchoManualMsg,
  EchoManualReq,
  MessagePackEchoMsg,
  MessagePackEchoReq,
  ProtobufEchoMsg,
  ProtobufEchoReq,
  RegistrationCodecNames,
  type CodecScenarioRes,
  type EchoRes
} from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createMainEndpoints(evidence: EvidenceStore, channel: ZLinkRouteClient): readonly HttpRoute[] {
  return [
    {
      method: 'POST',
      path: '/registration/auto',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoAutoReq('rc-a1'))
          .submit<EchoRes>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new EchoAutoMsg('cmd-rc-a1', 'rc-a1-send'))
          .submit();
        return reply;
      }
    },
    {
      method: 'POST',
      path: '/registration/attribute',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoAttrReq('rc-a2'))
          .submit<EchoRes>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new EchoAttrMsg('cmd-rc-a2', 'rc-a2-send'))
          .submit();
        return reply;
      }
    },
    {
      method: 'POST',
      path: '/registration/manual',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoManualReq('rc-a3'))
          .submit<EchoRes>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new EchoManualMsg('cmd-rc-a3', 'rc-a3-send'))
          .submit();
        return reply;
      }
    },
    {
      method: 'POST',
      path: '/registration/di-filter-order',
      handle: async () => {
        const first = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoDiReq('rc-a4-1'))
          .submit<EchoRes>();
        const second = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoDiReq('rc-a4-2'))
          .submit<EchoRes>();
        return [first, second];
      }
    },
    {
      method: 'POST',
      path: '/registration/filter-order',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoManualReq('rc-a5'))
          .submit<EchoRes>();
        evidence.add(`filter-reply|value=${reply.value}|content=${reply.contentType}`);
        return reply;
      }
    },
    {
      method: 'POST',
      path: '/codec/json',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoJsonReq('rc-b1'))
          .submit<EchoRes>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new EchoJsonMsg('cmd-rc-b1', 'rc-b1-send'))
          .submit();
        evidence.add(`codec-reply|codec=json|value=${reply.value}|content=${reply.contentType}`);
        return reply;
      }
    },
    {
      method: 'POST',
      path: '/codec/protobuf',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new ProtobufEchoReq('rc-b2'))
          .submit<ProtobufEchoReq>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new ProtobufEchoMsg('rc-b2-send'))
          .submit();
        evidence.add(`codec-reply|codec=protobuf|value=${reply.value}`);
        return {
          value: reply.value.replace(/\|content:.+$/, ''),
          contentType: reply.value.includes('content:application/x-protobuf') ? 'application/x-protobuf' : '<missing>'
        } satisfies CodecScenarioRes;
      }
    },
    {
      method: 'POST',
      path: '/codec/msgpack',
      handle: async () => {
        const reply = await channel.requestToChannel(RegistrationCodecNames.channel, new MessagePackEchoReq('rc-b3'))
          .submit<MessagePackEchoReq>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new MessagePackEchoMsg('cmd-rc-b3', 'rc-b3-send'))
          .submit();
        evidence.add(`codec-reply|codec=msgpack|value=${reply.value}`);
        return {
          value: reply.value.replace(/\|content:.+$/, ''),
          contentType: reply.value.includes('content:application/x-msgpack') ? 'application/x-msgpack' : '<missing>'
        } satisfies CodecScenarioRes;
      }
    },
    {
      method: 'POST',
      path: '/codec/roundtrip',
      handle: async () => {
        const json = await channel.requestToChannel(RegistrationCodecNames.channel, new EchoJsonReq('rc-b1'))
          .submit<EchoRes>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new EchoJsonMsg('cmd-rc-b1', 'rc-b1-send'))
          .submit();

        const protobuf = await channel.requestToChannel(RegistrationCodecNames.channel, new ProtobufEchoReq('rc-b2'))
          .submit<ProtobufEchoReq>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new ProtobufEchoMsg('rc-b2-send'))
          .submit();

        const messagePack = await channel.requestToChannel(RegistrationCodecNames.channel, new MessagePackEchoReq('rc-b3'))
          .submit<MessagePackEchoReq>();
        await channel.sendToChannel(RegistrationCodecNames.channel, new MessagePackEchoMsg('cmd-rc-b3', 'rc-b3-send'))
          .submit();

        evidence.add(`codec-reply|codec=json|value=${json.value}|content=${json.contentType}`);
        evidence.add(`codec-reply|codec=protobuf|value=${protobuf.value}`);
        evidence.add(`codec-reply|codec=msgpack|value=${messagePack.value}`);
        return {
          json,
          protobufValue: protobuf.value,
          messagePackValue: messagePack.value
        } satisfies CodecScenarioRes;
      }
    }
  ];
}
