import fs from 'node:fs';
import path from 'node:path';
import { QuestDomain } from '../../QuestMission/Domain/quest-domain';
import { QuestIds } from '../../../Shared/Contracts/messages';
import type {
  GameplayEventEnvelope,
  GetGameplaySnapshotRes,
  QuestProgress,
  StoredQuestEvent
} from '../../../Shared/Contracts/messages';
import type { ProposedQuestEvent } from '../../QuestMission/Domain/quest-domain';

type PlayerFacts = {
  kills: Record<string, number>;
  items: Record<string, number>;
  completedMissionIds: string[];
  unlockedFeatureIds: string[];
  enteredAreaIds: string[];
  snapshotVersion: number;
};

type GameplayState = {
  eventsByKey: Partial<Record<string, GameplayEventEnvelope>>;
  facts: PlayerFacts;
  evidence: string[];
};

type QuestEventState = {
  events: StoredQuestEvent[];
  ownerLifecycle: string[];
  evidence: string[];
};

type ReadModelState = {
  projections: QuestProgress[];
  evidence: string[];
};

const emptyGameplay = (): GameplayState => ({
  eventsByKey: {},
  facts: { kills: {}, items: {}, completedMissionIds: [], unlockedFeatureIds: [], enteredAreaIds: [], snapshotVersion: 0 },
  evidence: []
});
const emptyEvents = (): QuestEventState => ({ events: [], ownerLifecycle: [], evidence: [] });
const emptyReadModel = (): ReadModelState => ({ projections: [], evidence: [] });

class GameplayStateStore {
  private readonly partitions: JsonPartitionStore<GameplayState>;
  constructor(workDir: string) {
    this.partitions = new JsonPartitionStore(path.join(workDir, 'gameplay-state'), emptyGameplay);
  }

  recordGameplayEvent(candidate: GameplayEventEnvelope): { event: GameplayEventEnvelope; recorded: boolean } {
    return this.partitions.update(candidate.playerId, (state) => {
      const existing = state.eventsByKey[candidate.payload.idempotencyKey];
      if (existing !== undefined) return { event: existing, recorded: false };
      state.eventsByKey[candidate.payload.idempotencyKey] = candidate;
      applyFacts(state.facts, candidate);
      addEvidence(state.evidence, `gameplay:${candidate.eventId}:${candidate.type}`);
      return { event: candidate, recorded: true };
    });
  }

  readGameplaySnapshot(playerId: string): GetGameplaySnapshotRes {
    const facts = this.partitions.read(playerId).facts;
    return {
      playerId,
      killCounts: Object.entries(facts.kills).map(([key, count]) => {
        const [monsterId, areaId] = key.split(':');
        return { monsterId, areaId, count };
      }),
      itemCounts: Object.entries(facts.items).map(([itemId, count]) => ({ itemId, count })),
      completedMissionIds: [...facts.completedMissionIds],
      unlockedFeatureIds: [...facts.unlockedFeatureIds],
      enteredAreaIds: [...facts.enteredAreaIds],
      snapshotVersion: facts.snapshotVersion
    };
  }

  killWithoutPublish(playerId: string): void {
    this.partitions.update(playerId, (state) => {
      state.facts.kills['wolf:forest'] = (state.facts.kills['wolf:forest'] ?? 0) + 1;
      state.facts.snapshotVersion += 1;
      addEvidence(state.evidence, `missed-publish:${playerId}`);
    });
  }

  evidence(playerId: string): string[] { return this.partitions.read(playerId).evidence; }
}

class QuestEventStore {
  private readonly partitions: JsonPartitionStore<QuestEventState>;
  constructor(workDir: string) {
    this.partitions = new JsonPartitionStore(path.join(workDir, 'quest-events'), emptyEvents);
  }

  append(playerId: string, proposed: ProposedQuestEvent[], owner: string): StoredQuestEvent[] {
    return this.partitions.update(playerId, (state) => {
      const appended: StoredQuestEvent[] = [];
      for (const event of proposed) {
        const stored: StoredQuestEvent = {
          ...event,
          payload: event.payload as Record<string, unknown>,
          version: nextVersion(state.events, playerId, event.questId),
          createdAtUnixMs: event.createdAtUnixMs
        };
        state.events.push(stored);
        appended.push(stored);
      }
      if (proposed.length > 0) addEvidence(state.evidence, `owner:${owner}:${playerId}`);
      return appended;
    });
  }

  read(playerId: string): StoredQuestEvent[] { return this.partitions.read(playerId).events; }

  rehydrate(playerId: string): StoredQuestEvent[] {
    return this.partitions.update(playerId, (state) => {
      state.ownerLifecycle.push(`${playerId}:rehydrate:${state.ownerLifecycle.length + 1}`);
      addEvidence(state.evidence, `rehydrate:${playerId}:${state.ownerLifecycle.length}`);
      return state.events.map(cloneEvent);
    });
  }

  closeOwner(playerId: string, owner: string): void {
    this.partitions.update(playerId, (state) => {
      state.ownerLifecycle.push(`${owner}:close:${playerId}`);
      addEvidence(state.evidence, `owner-close:${owner}:${playerId}`);
    });
  }

  readQuestEventNames(): string[] {
    return this.partitions.keys().flatMap((playerId) => this.read(playerId).map((event) => event.type));
  }

  evidence(playerId: string): { evidence: string[]; ownerLifecycle: string[] } {
    const state = this.partitions.read(playerId);
    return { evidence: state.evidence, ownerLifecycle: state.ownerLifecycle };
  }
}

class QuestReadModelStore {
  private readonly partitions: JsonPartitionStore<ReadModelState>;
  constructor(workDir: string) {
    this.partitions = new JsonPartitionStore(path.join(workDir, 'quest-read-model'), emptyReadModel);
  }

  readProjection(playerId: string): QuestProgress[] {
    return this.partitions.read(playerId).projections.map((progress) => ({ ...progress }));
  }

  project(playerId: string, events: StoredQuestEvent[]): QuestProgress[] {
    const projection = QuestDomain.project(playerId, events);
    this.partitions.update(playerId, (state) => { state.projections = projection.map((progress) => ({ ...progress })); });
    return projection;
  }

  deleteProjection(playerId: string, questId: string): void {
    this.partitions.update(playerId, (state) => {
      state.projections = state.projections.filter((progress) => progress.questId !== questId);
      addEvidence(state.evidence, `projection-delete:${playerId}:${questId}`);
    });
  }

  rebuildProjection(playerId: string, questId: string, events: StoredQuestEvent[]): QuestProgress {
    const rebuilt = QuestDomain.project(playerId, events).find((progress) => progress.questId === questId);
    if (rebuilt === undefined) throw new Error(`Quest stream was not found for ${playerId}/${questId}.`);
    this.partitions.update(playerId, (state) => {
      state.projections = state.projections.filter((progress) => progress.questId !== questId);
      state.projections.push({ ...rebuilt });
      addEvidence(state.evidence, `projection-rebuild:${playerId}:${questId}:version=${rebuilt.version}`);
    });
    return rebuilt;
  }

  evidence(playerId: string): string[] { return this.partitions.read(playerId).evidence; }
}

class GameQuestSelfCheckStore {
  constructor(
    private readonly gameplay: GameplayStateStore,
    private readonly events: QuestEventStore,
    private readonly readModel: QuestReadModelStore
  ) {}

  assertServerEvidence(): { passed: boolean; evidence: string[] } {
    const aliceEvents = this.events.read('player-alice');
    const bobEvents = this.events.read('player-bob');
    const aliceEventEvidence = this.events.evidence('player-alice');
    const evidence = [
      ...this.gameplay.evidence('player-alice'), ...this.gameplay.evidence('player-bob'),
      ...aliceEventEvidence.evidence, ...this.events.evidence('player-bob').evidence,
      ...this.readModel.evidence('player-alice'), ...this.readModel.evidence('player-bob')
    ];
    const rewardCount = aliceEvents.filter((event) => event.questId === QuestIds.FirstHunt && event.type === 'QuestRewardGranted').length;
    const sourceCount = aliceEvents.filter((event) => event.sourceEventId === 'player-alice-kill-3').length;
    const bobReconcile = bobEvents.filter((event) => event.questId === QuestIds.FirstHunt &&
      ['QuestReconciled', 'QuestCompleted', 'QuestRewardGranted'].includes(event.type));
    const passed = rewardCount === 1 && sourceCount === 3 &&
      aliceEventEvidence.ownerLifecycle.filter((entry) => entry.includes(':rehydrate:')).length >= 2 &&
      bobReconcile.map((event) => event.type).join(',') === 'QuestReconciled,QuestCompleted,QuestRewardGranted' &&
      evidence.some((entry) => /^owner:mission-[ab]:player-alice$/.test(entry)) &&
      evidence.some((entry) => /^owner:mission-[ab]:player-bob$/.test(entry)) &&
      evidence.some((entry) => /^owner-close:mission-[ab]:player-alice$/.test(entry)) &&
      ['missed-publish:player-bob', 'projection-rebuild:player-bob:herb-gathering']
        .every((entry) => evidence.some((candidate) => candidate === entry || candidate.startsWith(`${entry}:`)));
    return { passed, evidence };
  }
}

class JsonPartitionStore<TState> {
  constructor(private readonly directory: string, private readonly createEmpty: () => TState) {
    fs.mkdirSync(directory, { recursive: true });
  }

  read(key: string): TState {
    const file = this.file(key);
    return fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) as TState : this.createEmpty();
  }

  update<TResult>(key: string, mutate: (state: TState) => TResult): TResult {
    const file = this.file(key);
    const lockFile = `${file}.lock`;
    const lock = acquireLock(lockFile);
    try {
      const state = this.read(key);
      const result = mutate(state);
      const temporary = `${file}.${process.pid}.${Math.random().toString(16).slice(2)}.tmp`;
      fs.writeFileSync(temporary, JSON.stringify(state, null, 2));
      fs.renameSync(temporary, file);
      return result;
    } finally {
      fs.closeSync(lock);
      fs.rmSync(lockFile, { force: true });
    }
  }

  keys(): string[] {
    return fs.readdirSync(this.directory).filter((name) => name.endsWith('.json'))
      .map((name) => Buffer.from(name.slice(0, -5), 'base64url').toString('utf8'));
  }

  private file(key: string): string { return path.join(this.directory, `${Buffer.from(key).toString('base64url')}.json`); }
}

function applyFacts(facts: PlayerFacts, event: GameplayEventEnvelope): void {
  if (event.type === 'MonsterKilled') facts.kills[event.payload.value] = (facts.kills[event.payload.value] ?? 0) + event.payload.count;
  else if (event.type === 'ItemCollected') facts.items[event.payload.value] = (facts.items[event.payload.value] ?? 0) + event.payload.count;
  else if (event.type === 'MissionCompleted') addUnique(facts.completedMissionIds, event.payload.value);
  else if (event.type === 'AreaEntered') addUnique(facts.enteredAreaIds, event.payload.value);
  else if (event.type === 'FeatureUnlocked') addUnique(facts.unlockedFeatureIds, event.payload.value);
  facts.snapshotVersion += 1;
}

function acquireLock(lockFile: string): number {
  for (let attempt = 0; attempt < 1000; attempt++) {
    try { return fs.openSync(lockFile, 'wx'); } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 2);
    }
  }
  throw new Error(`Timed out acquiring state lock '${lockFile}'.`);
}

function nextVersion(events: StoredQuestEvent[], playerId: string, questId: string): number {
  return events.filter((event) => event.playerId === playerId && event.questId === questId).length + 1;
}
function addEvidence(values: string[], value: string): void { if (!values.includes(value)) values.push(value); }
function addUnique(values: string[], value: string): void { if (!values.includes(value)) values.push(value); }
function cloneEvent(event: StoredQuestEvent): StoredQuestEvent { return { ...event, payload: { ...event.payload } }; }

export { GameQuestSelfCheckStore, GameplayStateStore, QuestEventStore, QuestReadModelStore };
