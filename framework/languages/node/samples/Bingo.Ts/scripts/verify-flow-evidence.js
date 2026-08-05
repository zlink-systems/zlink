#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const logDir = process.argv[2];
if (!logDir) throw new Error('Usage: verify-flow-evidence.js <log-dir>');

const read = (name) => fs.readFileSync(path.join(logDir, name), 'utf8').split(/\r?\n/);
const flow = (line) => line.match(/\bflow=([^ ]+)/)?.[1];
const flows = (lines, predicate) => new Set(lines.filter(predicate).map(flow).filter(Boolean));
const same = (left, right) => left.size === right.size && [...left].every((value) => right.has(value));
const requireSame = (label, left, right) => {
  if (left.size === 0 || !same(left, right)) {
    throw new Error(`${label} flow mismatch: left=${[...left].join(',')} right=${[...right].join(',')}`);
  }
};
const requireOnePerFlow = (label, lines, expectedFlows) => {
  const counts = new Map();
  for (const line of lines) {
    const id = flow(line);
    if (id !== undefined) counts.set(id, (counts.get(id) ?? 0) + 1);
  }
  for (const id of expectedFlows) {
    if (counts.get(id) !== 1) {
      throw new Error(`${label} expected one event for flow=${id}, actual=${counts.get(id) ?? 0}`);
    }
  }
};

const session = read('flow-session.log');
const play = read('flow-play.log');
const client = read('client.log');
const submit = (phase) => flows(session, (line) =>
  line.includes(`phase=${phase}`) && line.includes('packet=SubmitBingoCardReq'));
const spotSubmit = (phase) => flows(play, (line) =>
  line.includes(`phase=${phase}`) && line.includes('surface=spotActor') && line.includes('packet=SubmitBingoCardReq'));
requireSame('Submit stream-to-Spot receive', submit('received'), spotSubmit('received'));
requireSame('Submit Spot receive-to-reply', spotSubmit('received'), spotSubmit('replied'));

const timerDrawDispatchLines = play.filter((line) =>
  line.includes('phase=sent') &&
  line.includes('surface=spotActor') &&
  line.includes('kind=actorSend') &&
  line.includes('packet=BingoNumberDrawnNotify') &&
  line.includes('origin=Timer'));
const timerDraws = flows(timerDrawDispatchLines, () => true);
const player1DrawLines = client.filter((line) =>
  line.includes('client=player-1') && line.includes('name=BingoNumberDrawnNotify'));
const player2DrawLines = client.filter((line) =>
  line.includes('client=player-2') && line.includes('name=BingoNumberDrawnNotify'));
const player1Draws = flows(player1DrawLines, () => true);
const player2Draws = flows(player2DrawLines, () => true);
requireSame('Timer draw dispatch-to-player-1 bound push', timerDraws, player1Draws);
requireSame('Timer draw dispatch-to-player-2 bound push', timerDraws, player2Draws);
requireOnePerFlow('player-1 draw push', player1DrawLines, timerDraws);
requireOnePerFlow('player-2 draw push', player2DrawLines, timerDraws);
for (const id of timerDraws) {
  const actorCounts = new Map();
  for (const line of timerDrawDispatchLines.filter((candidate) => flow(candidate) === id)) {
    const actor = line.match(/\bactor=([^ ]+)/)?.[1];
    if (actor !== undefined) actorCounts.set(actor, (actorCounts.get(actor) ?? 0) + 1);
  }
  if (actorCounts.size !== 2 || actorCounts.get('player-1') !== 1 || actorCounts.get('player-2') !== 1) {
    throw new Error(
      `Timer draw flow=${id} expected one dispatch per player: ` +
      `actors=${[...actorCounts].map(([actor, count]) => `${actor}:${count}`).join(',')}`
    );
  }
}

const timerReceived = flows(play, (line) =>
  line.includes('phase=received') && line.includes('packet=BingoRewardAcquiredEvent') && line.includes('origin=Timer'));
const timerDispatched = flows(play, (line) =>
  line.includes('phase=dispatched') && line.includes('packet=BingoRewardAcquiredEvent') && line.includes('origin=Timer'));
const boundPush = flows(client, (line) =>
  line.includes('client=observer') && line.includes('name=BingoRewardAnnouncedNotify'));
requireSame('Timer receive-to-dispatch', timerReceived, timerDispatched);
requireSame('Timer dispatch-to-bound push', timerDispatched, boundPush);

console.log('flow-evidence=verified');
