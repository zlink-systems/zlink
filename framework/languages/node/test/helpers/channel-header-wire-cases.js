'use strict';

// Representative channel-header and service-wire encodes. The wire-bytes test
// compares this output with golden bytes captured from origin/main; the capture
// script runs the same function against a main build (never regenerate the
// golden file from the code under test).
const KIND = { Request: 1, Response: 2, Command: 3, Publish: 4, Error: 5 };
const FLOW = { flowId: '01992176-0000-7000-8000-000000000001', flowOrigin: 'Application' };
const TIMER_FLOW = { flowId: '01992176-0000-7000-8000-0000000000ff', flowOrigin: 'Timer' };
const CORRELATION = '00112233445566778899aabbccddeeff';
const MULTIPART_PACKET_NAME_ALTERNATE = '가';

const DEADLINES = [
  ['epoch+1ms', 1],
  ['ms-9', 9],
  ['ms-10', 10],
  ['ms-99', 99],
  ['ms-100', 100],
  ['second-edge', 1000],
  ['ms-999', 1999],
  ['y2026-ms005', Date.parse('2026-09-10T00:00:00.005Z')],
  ['y2026-ms050', Date.parse('2026-09-10T00:00:00.050Z')],
  ['y2026-ms500', Date.parse('2026-09-10T12:34:56.500Z')],
  ['y2026-ms999', Date.parse('2026-09-10T12:34:56.999Z')],
  ['leap-day', Date.parse('2028-02-29T23:59:59.999Z')],
  ['year-end', Date.parse('2026-12-31T23:59:59.999Z')],
  ['y9999-last', 253402300799999],
  ['y10000-first', 253402300800000],
  ['max-date', 8640000000000000]
];

function buildCases(dist) {
  const env = require(`${dist}/runtime/channels/channel-envelope`);
  const wire = require(`${dist}/runtime/foundation/service-wire-m6a-codec`);
  const { runWithFlow } = require(`${dist}/runtime/diagnostics/flow-context`);
  const bytes = (part) =>
    typeof part === 'string' ? Buffer.from(part) : Buffer.isBuffer(part) ? part : Buffer.from(part.data());
  const views = (parts) => parts.map((part) => (typeof part === 'string' ? part : bytes(part)));
  const cases = [];

  function add(name, makeParts, packet = ['multipart', 'x']) {
    const parts = makeParts();
    const headerBytes = bytes(parts[0]);
    const entry = {
      name,
      header: headerBytes.toString('hex'),
      frame: wire.encodeMultipartApplicationPayload(views(parts), packet[0], packet[1]).toString('hex'),
      requestHeader: wire.encodeNodeRequestHeader(0x0102030405060708n).toString('hex'),
      decoded: JSON.stringify(decodeHeader(env, headerBytes))
    };
    cases.push(entry);
  }

  const request = (kind, channel, packet, payload, deadline, topic, id, createFlow, metadata) =>
    () =>
      env.encodeChannelEnvelopePartsAtDeadline(
        kind, channel, packet, payload, deadline, topic, undefined, id, createFlow, metadata
      );
  const withFlow = (flow, make) => () => runWithFlow(flow, make);

  add('request-deadline-flow', withFlow(FLOW, request(KIND.Request, 'echo', 'Echo', { body: 'hello' }, DEADLINES[7][1], undefined, CORRELATION, true)));
  add('request-no-deadline-no-flow', request(KIND.Request, 'echo', 'Echo', { body: 'hello' }, undefined, undefined, CORRELATION, false));
  add('request-timer-flow-binary', withFlow(TIMER_FLOW, request(KIND.Request, 'bench', 'BenchGameMessage', Buffer.alloc(1100, 0x61), DEADLINES[9][1], undefined, 'c-1', true)));
  add('request-metadata-unsorted-multibyte', request(KIND.Request, 'echo', 'Echo', 'text', DEADLINES[10][1], undefined, CORRELATION, false,
    new Map([['zeta', 'z'], ['é', 'accent'], ['alpha', ''], ['Beta', 'b'], ['가', '나'], ['a', '1']])));
  add('request-empty-metadata-value-flow', withFlow(FLOW, request(KIND.Request, 'echo', 'Echo', { body: 'x' }, DEADLINES[11][1], undefined, CORRELATION, true,
    new Map([['k', '']]))));
  add('send-metadata-flow', withFlow(FLOW, request(KIND.Command, 'echo', 'Echo', { body: 'send' }, undefined, undefined, undefined, true,
    new Map([['b', '2'], ['a', '1']]))));
  add('send-no-flow-deadline', request(KIND.Command, 'echo', 'Echo', { body: 'send' }, DEADLINES[8][1], undefined, undefined, false));
  add('publish-topic', request(KIND.Publish, 'events', 'Evt', { v: 1 }, undefined, 'topic/a', undefined, false));
  add('publish-topic-flow', withFlow(FLOW, () =>
    env.encodeChannelPublishEnvelopeParts('events', 'topic/b', 'Evt', { v: 2 }, undefined, true, new Map([['m', 'v']]))));
  add('request-one-letter-channel', request(KIND.Request, 'x', 'Echo', { body: 1 }, undefined, undefined, 'c', false));
  for (const [label, deadline] of DEADLINES) {
    add(`deadline-${label}`, request(KIND.Request, 'echo', 'Echo', { body: 'd' }, deadline, undefined, CORRELATION, false));
  }

  const replyRequest = (flow, correlationId = CORRELATION) => ({
    kind: KIND.Request, channelName: 'echo', messageName: 'Echo', contentType: 'application/json',
    correlationId, deadline: null, topic: null, metadata: {}, flowId: flow?.flowId, flowOrigin: flow?.flowOrigin
  });
  add('reply-flow', () => env.encodeChannelReplyParts(replyRequest(FLOW), { body: 'reply' }));
  add('reply-no-flow', () => env.encodeChannelReplyParts(replyRequest(undefined), { body: 'reply' }));
  add('reply-binary-empty', () => env.encodeChannelReplyParts(replyRequest(TIMER_FLOW, 'c-1'), undefined));
  add('reply-bench', () => env.encodeChannelReplyParts({ ...replyRequest(undefined, 'c-1'), channelName: 'bench', messageName: 'BenchGameMessage' }, Buffer.alloc(1100, 0x61)));
  add('error-reply-origin-metadata', () =>
    env.encodeChannelErrorReplyParts(replyRequest(FLOW), new Error('boom'), { 'zlink.origin': 'framework', extra: 'e' }));
  add('error-reply-no-metadata-no-flow', () => env.encodeChannelErrorReplyParts(replyRequest(undefined), new Error('boom')));

  // More distinct (kind, channel, packet, content type) tuples than the prefix
  // cache keeps, visited in an order that evicts and re-learns entries.
  for (let index = 0; index < 12; index += 1) {
    add(`rotation-${index}`, request(KIND.Request, `ch${index}`, `Msg${index}`, { i: index }, undefined, undefined, `c${index}`, false));
  }

  // Frame variants that exercise the text8 field cache and the multipart writer.
  const frames = [];
  const partsSets = [
    ['empty string', [Buffer.from('h'), '']],
    ['multibyte string', [Buffer.from('h'), '한글 text ✓']],
    ['lone surrogate string', [Buffer.from('h'), 'a\ud800b']],
    ['buffer + string + buffer', [Buffer.from('h'), 'mid', Buffer.alloc(300, 7)]],
    ['single part', [Buffer.alloc(0)]],
    ['one string only', ['only']]
  ];
  for (const names of [['multipart', 'x'], [MULTIPART_PACKET_NAME_ALTERNATE, 'c'], ['multipart', 'x']]) {
    for (const [label, parts] of partsSets) {
      frames.push({
        name: `${names[0]}/${names[1]}/${label}`,
        frame: wire.encodeMultipartApplicationPayload(parts, names[0], names[1]).toString('hex'),
        bare: wire.encodeMultipartPayload(parts).toString('hex')
      });
    }
  }
  return { cases, frames, replyHeader: wire.encodeReplyHeader(0x0102030405060708n, 0, 0).toString('hex') };
}

function decodeHeader(env, headerBytes) {
  const header = env.decodeChannelHeader([{ data: () => headerBytes }]);
  return { ...header, metadata: Object.entries(header.metadata).sort() };
}

module.exports = { buildCases, decodeHeader };
