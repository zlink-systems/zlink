import {
  PacketNames,
  QuestIds,
  QuestStatuses,
  collectItemReq,
  completeMissionReq,
  enterAreaReq,
  getQuestProgressReq,
  joinSessionReq,
  killMonsterReq,
  syncQuestProgressReq,
  unlockFeatureReq
} from '../Shared/Contracts/messages';
import type { BrowserHttpClient } from '../../browser-client-runtime';
import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import type {
  CollectItemRes,
  CompleteMissionRes,
  EnterAreaRes,
  GameQuestServerAssertRes,
  GetGameplaySnapshotRes,
  GetQuestProgressRes,
  JoinSessionRes,
  KillMonsterRes,
  QuestCompletedNotify,
  QuestProgress,
  QuestProgressNotify,
  SyncQuestProgressRes,
  UnlockFeatureRes
} from '../Shared/Contracts/messages';

class GameQuestClientScenario {
  async run(
    apiA: BrowserHttpClient,
    apiB: BrowserHttpClient,
    missionA: BrowserHttpClient,
    missionB: BrowserHttpClient,
    apiAStream: ZlinkStreamConnector,
    apiBStream: ZlinkStreamConnector,
    apiBReconnectStream: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    await Promise.all([apiAStream.connect(signal), apiBStream.connect(signal)]);
    const [joined, bobJoined] = await Promise.all([
      apiAStream.request(joinSessionReq('player-alice'), Object)
        .packetName(PacketNames.joinSessionReq)
        .submit<JoinSessionRes>(signal),
      apiBStream.request(joinSessionReq('player-bob'), Object)
        .packetName(PacketNames.joinSessionReq)
        .submit<JoinSessionRes>(signal)
    ]);
    zlinkStreamAssert.ensure(joined.activeQuests.length === 0, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(bobJoined.activeQuests.length === 0, 'Sample scenario assertion failed.');

    const wrongArea = await apiAStream.request(killMonsterReq('player-alice', 'wolf', 'desert', 'kill-wrong-area'), Object)
      .packetName(PacketNames.killMonsterReq)
      .submit<KillMonsterRes>(signal);
    zlinkStreamAssert.ensure(wrongArea.eventId === 'player-alice-kill-wrong-area', 'Sample scenario assertion failed.');
    await apiAStream.expectNone(PacketNames.questProgressNotify).within(250).run(signal);
    await zlinkStreamAssert.expectFailure(
      async () => {
        await apiBStream.request(collectItemReq('player-bob', 'healing-herb', -1, 'invalid-negative-count'), Object)
          .packetName(PacketNames.collectItemReq)
          .submit<CollectItemRes>(signal);
      }
    );
    await apiBStream.expectNone(PacketNames.questProgressNotify).within(250).run(signal);

    const firstProgress = apiAStream.waitFor<QuestProgressNotify>(PacketNames.questProgressNotify)
      .where((message) => message.payload.playerId === 'player-alice')
      .submit(signal);
    const firstHerbProgress = apiBStream.waitFor<QuestProgressNotify>(PacketNames.questProgressNotify)
      .where((message) => message.payload.playerId === 'player-bob')
      .submit(signal);
    const [firstKill, offlineItem] = await Promise.all([
      apiAStream.request(killMonsterReq('player-alice', 'wolf', 'forest', 'kill-1'), Object)
        .packetName(PacketNames.killMonsterReq)
        .submit<KillMonsterRes>(signal),
      apiBStream.request(collectItemReq('player-bob', 'healing-herb', 1, 'herb-1'), Object)
        .packetName(PacketNames.collectItemReq)
        .submit<CollectItemRes>(signal)
    ]);
    zlinkStreamAssert.ensure(firstKill.eventId === 'player-alice-kill-1', 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(offlineItem.eventId === 'player-bob-herb-1', 'Sample scenario assertion failed.');
    const firstProgressPush = await firstProgress;
    const firstHerbPush = await firstHerbProgress;
    zlinkStreamAssert.ensure(firstProgressPush.payload.progress.questId === QuestIds.FirstHunt, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(firstProgressPush.payload.progress.currentCount === 1, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(!('targetConnectionId' in firstProgressPush.payload), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(firstHerbPush.payload.progress.questId === QuestIds.HerbGathering, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(firstHerbPush.payload.progress.currentCount === 1, 'Sample scenario assertion failed.');
    const completeFirstHunt = waitForQuestCompletion(apiAStream, QuestIds.FirstHunt, signal);
    await apiAStream.request(killMonsterReq('player-alice', 'wolf', 'forest', 'kill-2'), Object)
      .packetName(PacketNames.killMonsterReq)
      .submit<KillMonsterRes>(signal);
    const thirdKill = await apiAStream.request(killMonsterReq('player-alice', 'wolf', 'forest', 'kill-3'), Object)
      .packetName(PacketNames.killMonsterReq)
      .submit<KillMonsterRes>(signal);
    zlinkStreamAssert.ensure(thirdKill.eventId === 'player-alice-kill-3', 'Sample scenario assertion failed.');
    const firstHuntPush = await completeFirstHunt;
    zlinkStreamAssert.ensure(firstHuntPush.payload.rewardGranted, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(!('targetConnectionId' in firstHuntPush.payload), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(firstHuntPush.payload.progress.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');

    const duplicate = await apiAStream.request(killMonsterReq('player-alice', 'wolf', 'forest', 'kill-3'), Object)
      .packetName(PacketNames.killMonsterReq)
      .submit<KillMonsterRes>(signal);
    zlinkStreamAssert.ensure(duplicate.eventId === thirdKill.eventId, 'Sample scenario assertion failed.');
    const afterCompletion = await apiAStream.request(killMonsterReq('player-alice', 'wolf', 'forest', 'kill-4'), Object)
      .packetName(PacketNames.killMonsterReq)
      .submit<KillMonsterRes>(signal);
    zlinkStreamAssert.ensure(afterCompletion.eventId === 'player-alice-kill-4', 'Sample scenario assertion failed.');
    await apiAStream.expectNone(PacketNames.questCompletedNotify).within(250).run(signal);

    const auctionComplete = waitForQuestCompletion(apiAStream, QuestIds.OpenAuction, signal);
    const auction = await apiAStream.request(unlockFeatureReq('player-alice', 'auction', 'unlock-auction'), Object)
      .packetName(PacketNames.unlockFeatureReq)
      .submit<UnlockFeatureRes>(signal);
    zlinkStreamAssert.ensure(auction.eventId === 'player-alice-unlock-auction', 'Sample scenario assertion failed.');
    const auctionCompletePush = await auctionComplete;
    zlinkStreamAssert.ensure(auctionCompletePush.payload.rewardGranted, 'Sample scenario assertion failed.');
    const snapshot = await apiA.post('/internal/snapshot')
      .body({ playerId: 'player-alice' })
      .fetch<GetGameplaySnapshotRes>();
    zlinkStreamAssert.ensure(snapshot.unlockedFeatureIds.includes('auction'), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(snapshot.killCounts.some((entry) => entry.monsterId === 'wolf' && entry.areaId === 'desert' && entry.count === 1), 'Sample scenario assertion failed.');

    await apiAStream.close();
    await apiBReconnectStream.connect(signal);
    const aliceRejoined = await apiBReconnectStream.request(joinSessionReq('player-alice'), Object)
      .packetName(PacketNames.joinSessionReq)
      .submit<JoinSessionRes>(signal);
    zlinkStreamAssert.ensure(aliceRejoined.activeQuests.some((progress) =>
      progress.questId === QuestIds.FirstHunt && progress.status === QuestStatuses.RewardGranted), 'Sample scenario assertion failed.');
    const beforeDeactivate = requireQuest(aliceRejoined.activeQuests, QuestIds.FirstHunt);

    const closeOwnerA = await missionA.post('/self-check/owner/player-alice/close').fetch<{ closed: boolean }>();
    const closeOwnerB = await missionB.post('/self-check/owner/player-alice/close').fetch<{ closed: boolean }>();
    zlinkStreamAssert.ensure(closeOwnerA.closed || closeOwnerB.closed, 'Sample scenario assertion failed.');

    // Close is one-way. Sync observes the completed owner lifecycle before
    // the next one-way action may start a new Instance activation.
    const closeSync = await apiBReconnectStream.request(syncQuestProgressReq('player-alice'), Object)
      .packetName(PacketNames.syncQuestProgressReq)
      .submit<SyncQuestProgressRes>(signal);
    const afterCloseFirstHunt = requireQuest(closeSync.updatedQuests, QuestIds.FirstHunt);
    zlinkStreamAssert.ensure(afterCloseFirstHunt.currentCount >= beforeDeactivate.currentCount, 'Sample scenario assertion failed.');

    const earlyRuins = await apiBReconnectStream.request(enterAreaReq('player-alice', 'ruins', 'enter-ruins-too-early'), Object)
      .packetName(PacketNames.enterAreaReq)
      .submit<EnterAreaRes>(signal);
    zlinkStreamAssert.ensure(earlyRuins.eventId === 'player-alice-enter-ruins-too-early', 'Sample scenario assertion failed.');
    const beforeTutorial = await getStreamProjection(apiBReconnectStream, 'player-alice', signal);
    zlinkStreamAssert.ensure(beforeTutorial.every((progress) => progress.questId !== QuestIds.RuinsExplorer), 'Sample scenario assertion failed.');

    const tutorialCompleted = waitForQuestCompletion(apiBReconnectStream, QuestIds.TutorialPath, signal);
    const tutorial = await apiBReconnectStream.request(completeMissionReq('player-alice', 'tutorial', 'mission-tutorial'), Object)
      .packetName(PacketNames.completeMissionReq)
      .submit<CompleteMissionRes>(signal);
    zlinkStreamAssert.ensure(tutorial.eventId === 'player-alice-mission-tutorial', 'Sample scenario assertion failed.');
    // Observe the existing projection through a typed request after the
    // one-way gameplay action before waiting for its actor notification.
    await getStreamProjection(apiBReconnectStream, 'player-alice', signal);
    await tutorialCompleted;
    const afterReactivate = await getStreamProjection(apiBReconnectStream, 'player-alice', signal);
    const restoredFirstHunt = requireQuest(afterReactivate, QuestIds.FirstHunt);
    zlinkStreamAssert.ensure(restoredFirstHunt.currentCount === afterCloseFirstHunt.currentCount, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(restoredFirstHunt.status === afterCloseFirstHunt.status, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(restoredFirstHunt.version === afterCloseFirstHunt.version, 'Sample scenario assertion failed.');
    const tutorialPath = requireQuest(afterReactivate, QuestIds.TutorialPath);
    zlinkStreamAssert.ensure(tutorialPath.currentCount === 2, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(tutorialPath.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');
    const ruinsBeforeFinalStep = requireQuest(afterReactivate, QuestIds.RuinsExplorer);
    zlinkStreamAssert.ensure(ruinsBeforeFinalStep.currentCount === 1, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(ruinsBeforeFinalStep.status === QuestStatuses.Active, 'Sample scenario assertion failed.');
    console.log('gamequest-rehydrate=completed');
    const ruinsCompleted = waitForQuestCompletion(apiBReconnectStream, QuestIds.RuinsExplorer, signal);
    const ruins = await apiBReconnectStream.request(enterAreaReq('player-alice', 'ruins', 'enter-ruins'), Object)
      .packetName(PacketNames.enterAreaReq)
      .submit<EnterAreaRes>(signal);
    zlinkStreamAssert.ensure(ruins.eventId === 'player-alice-enter-ruins', 'Sample scenario assertion failed.');
    // Observe the existing projection through a typed request after the
    // one-way gameplay action before waiting for its actor notification.
    await getStreamProjection(apiBReconnectStream, 'player-alice', signal);
    const ruinsPush = await ruinsCompleted;
    zlinkStreamAssert.ensure(ruinsPush.payload.progress.currentCount === 2, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(ruinsPush.payload.progress.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');

    const bobProgress = await waitForStreamProjection(
      apiBStream,
      'player-bob',
      (p) => p.questId === QuestIds.HerbGathering && p.currentCount === 1,
      signal
    );
    zlinkStreamAssert.ensure(bobProgress.some((p) => p.questId === QuestIds.HerbGathering && p.currentCount === 1), 'Sample scenario assertion failed.');

    const herbCompleted = waitForQuestCompletion(apiBStream, QuestIds.HerbGathering, signal);
    const onlineItem = await apiBStream.request(collectItemReq('player-bob', 'healing-herb', 4, 'herb-2'), Object)
      .packetName(PacketNames.collectItemReq)
      .submit<CollectItemRes>(signal);
    zlinkStreamAssert.ensure(onlineItem.eventId === 'player-bob-herb-2', 'Sample scenario assertion failed.');
    const herbPush = await herbCompleted;
    zlinkStreamAssert.ensure(herbPush.payload.playerId === 'player-bob', 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(herbPush.payload.rewardGranted, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(herbPush.payload.progress.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');

    const bobAuctionCompleted = waitForQuestCompletion(apiBStream, QuestIds.OpenAuction, signal);
    await apiBStream.request(unlockFeatureReq('player-bob', 'auction', 'bob-unlock-auction'), Object)
      .packetName(PacketNames.unlockFeatureReq)
      .submit<UnlockFeatureRes>(signal);
    await bobAuctionCompleted;
    const beforeRebuild = await getStreamProjection(apiBStream, 'player-bob', signal);
    const auctionBeforeRebuild = requireQuest(beforeRebuild, QuestIds.OpenAuction);
    const herbBeforeRebuild = requireQuest(beforeRebuild, QuestIds.HerbGathering);

    const deleted = await apiA.post(`/self-check/projection/player-bob/${QuestIds.HerbGathering}/delete`).submitRaw();
    zlinkStreamAssert.ensure(deleted.status >= 200 && deleted.status < 300, 'Sample scenario assertion failed.');
    const missingProjection = await getStreamProjection(apiBStream, 'player-bob', signal);
    zlinkStreamAssert.ensure(missingProjection.every((progress) => progress.questId !== QuestIds.HerbGathering), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(requireQuest(missingProjection, QuestIds.OpenAuction).version === auctionBeforeRebuild.version, 'Sample scenario assertion failed.');
    const rebuilt = await apiA.post(`/self-check/projection/player-bob/${QuestIds.HerbGathering}/rebuild`).fetch<QuestProgress>();
    zlinkStreamAssert.ensure(rebuilt.questId === QuestIds.HerbGathering, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(rebuilt.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(rebuilt.version === herbBeforeRebuild.version, 'Sample scenario assertion failed.');
    const rebuiltProjection = await getStreamProjection(apiBStream, 'player-bob', signal);
    zlinkStreamAssert.ensure(rebuiltProjection.some((progress) =>
      progress.questId === QuestIds.HerbGathering && progress.status === QuestStatuses.RewardGranted), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(requireQuest(rebuiltProjection, QuestIds.OpenAuction).version === auctionBeforeRebuild.version, 'Sample scenario assertion failed.');

    for (const [key, expected] of [['bob-kill-1', 1], ['bob-kill-2', 2]] as const) {
      await apiBStream.request(killMonsterReq('player-bob', 'wolf', 'forest', key), Object)
        .packetName(PacketNames.killMonsterReq)
        .submit<KillMonsterRes>(signal);
      const progress = await waitForStreamProjection(apiBStream, 'player-bob', (candidate) =>
        candidate.questId === QuestIds.FirstHunt && candidate.currentCount === expected, signal);
      zlinkStreamAssert.ensure(requireQuest(progress, QuestIds.FirstHunt).currentCount === expected, 'Sample scenario assertion failed.');
    }
    const bobMissed = await apiB.post('/self-check/gameplay/kill-without-publish/player-bob').submitRaw();
    zlinkStreamAssert.ensure(bobMissed.status >= 200 && bobMissed.status < 300, 'Sample scenario assertion failed.');
    const bobReconcileCompleted = waitForQuestCompletion(apiBStream, QuestIds.FirstHunt, signal);
    const bobSync = await apiBStream.request(syncQuestProgressReq('player-bob'), Object)
      .packetName(PacketNames.syncQuestProgressReq)
      .submit<SyncQuestProgressRes>(signal);
    const bobReconciled = requireQuest(bobSync.updatedQuests, QuestIds.FirstHunt);
    zlinkStreamAssert.ensure(bobReconciled.currentCount === 3, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(bobReconciled.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');
    const bobReconcilePush = await bobReconcileCompleted;
    zlinkStreamAssert.ensure(bobReconcilePush.payload.progress.status === QuestStatuses.RewardGranted, 'Sample scenario assertion failed.');

    const missed = await apiB.post('/self-check/gameplay/kill-without-publish/player-alice').submitRaw();
    zlinkStreamAssert.ensure(missed.status >= 200 && missed.status < 300, 'Sample scenario assertion failed.');
    const sync = await apiBReconnectStream.request(syncQuestProgressReq('player-alice'), Object)
      .packetName(PacketNames.syncQuestProgressReq)
      .submit<SyncQuestProgressRes>(signal);
    zlinkStreamAssert.ensure(sync.updatedQuests.some((progress) =>
      progress.questId === QuestIds.FirstHunt && progress.currentCount >= 4), 'Sample scenario assertion failed.');
    const reconciled = await waitForStreamProjection(apiBReconnectStream, 'player-alice', (progress) =>
      progress.questId === QuestIds.FirstHunt && progress.currentCount >= 4, signal);
    zlinkStreamAssert.ensure(reconciled.some((progress) => progress.questId === QuestIds.FirstHunt && progress.currentCount >= 4), 'Sample scenario assertion failed.');

    await apiBReconnectStream.close();
    const assertion = await waitForServerAssertion(apiA, signal);
    zlinkStreamAssert.ensure(assertion.passed, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(assertion.evidence.some((entry) => /^owner:mission-[ab]:player-alice$/.test(entry)), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(assertion.evidence.some((entry) => /^owner:mission-[ab]:player-bob$/.test(entry)), 'Sample scenario assertion failed.');
    console.log('gamequest-concurrent-owners=completed');
    console.log('gamequest-server-evidence=completed');
  }
}

async function waitForQuestCompletion(
  connector: ZlinkStreamConnector,
  questId: string,
  signal?: AbortSignal
): Promise<{ payload: QuestCompletedNotify }> {
  try {
    return await connector.waitFor<QuestCompletedNotify>(PacketNames.questCompletedNotify)
      .where((message) => message.payload.progress.questId === questId)
      .submit(signal);
  } catch (cause) {
    throw new Error(`Quest completion notification was not received for '${questId}'.`, { cause });
  }
}

async function waitForServerAssertion(api: BrowserHttpClient, signal?: AbortSignal): Promise<GameQuestServerAssertRes> {
  let last: GameQuestServerAssertRes | undefined;
  for (let i = 0; i < 80; i++) {
    last = await api.post('/self-check/assert').fetch<GameQuestServerAssertRes>();
    if (last.passed) {
      return last;
    }
    await delay(50, signal);
  }
  return last ?? { passed: false, evidence: [] };
}

async function waitForStreamProjection(
  connector: ZlinkStreamConnector,
  playerId: string,
  predicate: (progress: QuestProgress) => boolean,
  signal?: AbortSignal
): Promise<QuestProgress[]> {
  for (let i = 0; i < 80; i++) {
    const projection = await getStreamProjection(connector, playerId, signal);
    if (projection.some(predicate)) {
      return projection;
    }
    await delay(50, signal);
  }
  return await getStreamProjection(connector, playerId, signal);
}

async function getStreamProjection(
  connector: ZlinkStreamConnector,
  playerId: string,
  signal?: AbortSignal
): Promise<QuestProgress[]> {
  const response = await connector.request(getQuestProgressReq(playerId), Object)
    .packetName(PacketNames.getQuestProgressReq)
    .submit<GetQuestProgressRes>(signal);
  return response.activeQuests;
}

function requireQuest(projection: QuestProgress[], questId: string): QuestProgress {
  const progress = projection.find((candidate) => candidate.questId === questId);
  if (progress === undefined) throw new Error(`Quest '${questId}' was not found in the projection.`);
  return progress;
}

function delay(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener('abort', () => {
      clearTimeout(timer);
      reject(new DOMException('Operation aborted.', 'AbortError'));
    }, { once: true });
  });
}

export { GameQuestClientScenario };
