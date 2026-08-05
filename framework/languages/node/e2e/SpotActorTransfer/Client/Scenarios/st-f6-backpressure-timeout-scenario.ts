// ST-F6: Request terminal across relocation 시나리오를 검증한다.
import type { ProbeReq, ProbeRes } from '../../Shared/messages.js';
import { SpotActorTransferNames, nodeA, nodeB, createSpot, createActor, joinActor, probeActor, waitEvidence, post, unique, require } from '../Support/scenario-support';

export async function runStF6(): Promise<void> {
  const replyActorId = unique('actor-handoff-gate-f6-reply');
  const replySpotId = unique('spot-handoff-request-reply');
  await createSpot(nodeB, replySpotId);
  await createActor(nodeA, replyActorId, SpotActorTransferNames.actorTypeStateful, 106);
  const replyJoin = joinActor(nodeA, replyActorId, {
    scenario: 'ST-F6',
    targetSpotId: replySpotId
  });
  await waitEvidence(nodeA, [`ST-F6|${replyActorId}|before_commit_gate|106`]);
  const inFlightReply = post<ProbeRes>(nodeA, `/actors/${replyActorId}/probe`, {
    scenario: 'ST-F6',
    marker: 'R1',
    requestTimeoutMs: 5000
  } satisfies ProbeReq);
  await waitEvidence(nodeA, [`ST-F6|${replyActorId}|handoff_backlog|0`]);
  const requestFrameEvidence = await waitEvidence(nodeA, [
    `ST-F6|${replyActorId}|handoff_request_frame|index=0|requestSeq=`
  ]);
  const requestFrame = requestFrameEvidence.find(
    (entry) => entry.actorId === replyActorId && entry.kind === 'handoff_request_frame'
  );
  require(
    requestFrame !== undefined && /requestSeq=\d+\|flags=[1-9]\d*/.test(requestFrame.value),
    'ST-F6 handoff evidence did not preserve request id and flags.'
  );
  await post(nodeA, `/transfer-gates/${replyActorId}/release`, {});
  require((await replyJoin).accepted, 'ST-F6 reply-correlation join failed.');
  const reply = await inFlightReply;
  require(
    reply.marker === 'R1' && reply.actorId === replyActorId && reply.nodeRid === 'actor-b',
    'ST-F6 in-flight request reply did not correlate to the original caller.'
  );
  const replyEvidence = await waitEvidence(nodeB, [
    `ST-F6|${replyActorId}|packet_handler|R1`,
    `ST-F6|${replyActorId}|request_reply|R1`
  ]);
  require(
    replyEvidence.filter((entry) => entry.actorId === replyActorId && entry.kind === 'request_reply').length === 1,
    'ST-F6 correlated request produced a duplicate reply.'
  );

  const timeoutActorId = unique('actor-handoff-gate-f6-timeout');
  const timeoutSpotId = unique('spot-handoff-request-timeout');
  await createSpot(nodeB, timeoutSpotId);
  await createActor(nodeA, timeoutActorId, SpotActorTransferNames.actorTypeStateful, 107);
  const timeoutJoin = joinActor(nodeA, timeoutActorId, {
    scenario: 'ST-F6',
    targetSpotId: timeoutSpotId
  });
  await waitEvidence(nodeA, [`ST-F6|${timeoutActorId}|before_commit_gate|107`]);
  const timedRequest = post<ProbeRes>(nodeA, `/actors/${timeoutActorId}/probe`, {
    scenario: 'ST-F6',
    marker: 'late',
    delayMs: 300,
    requestTimeoutMs: 100
  } satisfies ProbeReq);
  await waitEvidence(nodeA, [`ST-F6|${timeoutActorId}|handoff_backlog|0`]);
  await post(nodeA, `/transfer-gates/${timeoutActorId}/release`, {});
  let timedOut = false;
  try {
    await timedRequest;
  } catch {
    timedOut = true;
  }
  require(timedOut, 'ST-F6 delayed in-flight request did not use the caller request timeout.');
  require((await timeoutJoin).accepted, 'ST-F6 timeout join failed.');
  await waitEvidence(nodeB, [`ST-F6|${timeoutActorId}|request_reply|late`]);
  const afterLateReply = await probeActor(nodeB, timeoutActorId, 'ST-F6', 'after-late');
  require(afterLateReply.marker === 'after-late', 'ST-F6 late reply disrupted the next request.');
}
