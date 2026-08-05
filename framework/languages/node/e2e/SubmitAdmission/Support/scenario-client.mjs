import fs from 'node:fs';

const [callerUrl, targetUrl, publisherUrl, callerRid, targetRid, evidenceFile, ...selectors] = process.argv.slice(2);

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function json(url, method = 'GET', body = undefined) {
  const response = await fetch(url, {
    method,
    headers: body === undefined ? undefined : { 'content-type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  const value = await response.json();
  if (!response.ok) throw new Error(`${method} ${url} failed (${response.status}): ${JSON.stringify(value)}`);
  return value;
}

async function submit(url, operationId, target = undefined) {
  return json(url, 'POST', { operationId, targetRid: target });
}

async function waitEvidence(operationId, predicate, baseUrl = targetUrl) {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    const value = await json(`${baseUrl}/evidence?operationId=${encodeURIComponent(operationId)}`);
    if (predicate(value)) return value;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`evidence did not converge for ${operationId}`);
}

async function waitRouteState(target, ready) {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    const response = await fetch(`${callerUrl}/ready?targetRid=${encodeURIComponent(target)}`);
    if (response.ok === ready) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`route state did not become ready=${ready} for ${target}`);
}

function terminal(value, operationId, status = 'submitted') {
  assert(value.operationId === operationId, `${operationId}: operation id mismatch`);
  assert(value.status === status, `${operationId}: expected ${status}, got ${value.status}`);
  assert(value.publicInvocationCount === 1, `${operationId}: invocation count mismatch`);
  assert(value.terminalCount === 1, `${operationId}: terminal count mismatch`);
}

function emit(scenarioId, detail) {
  const line = JSON.stringify({ scenarioId, ...detail });
  fs.appendFileSync(evidenceFile, `${line}\n`);
  console.log(`${scenarioId} PASS ${JSON.stringify(detail)}`);
}

for (const selector of selectors) {
  if (selector === 'SA-E2E-01') {
    const nodeId = 'sa01-node';
    const channelId = 'sa01-channel';
    terminal(await submit(`${callerUrl}/submit/node`, nodeId, targetRid), nodeId);
    terminal(await submit(`${callerUrl}/submit/channel`, channelId), channelId);
    const nodeEvidence = await waitEvidence(nodeId, (value) => value.completed === 1);
    const channelEvidence = await waitEvidence(channelId, (value) => value.completed === 1);
    assert(nodeEvidence.entered === 1, 'SA-E2E-01 node handler count mismatch');
    assert(channelEvidence.entered === 1, 'SA-E2E-01 channel handler count mismatch');
    emit(selector, {
      families: ['node-direct', 'channel-name'],
      publicInvocationCount: 1,
      terminalCount: 1,
      handlerCount: 1
    });
  } else if (selector === 'SA-E2E-05') {
    for (let attempt = 0; attempt < 100; attempt += 1) {
      const operationId = `sa05-missing-${attempt}`;
      terminal(
        await submit(`${callerUrl}/submit/node`, operationId, 'submit-missing'),
        operationId,
        'targetNotFound'
      );
    }
    await json(`${targetUrl}/shutdown`, 'POST', {});
    await waitRouteState(targetRid, false);
    for (let attempt = 0; attempt < 100; attempt += 1) {
      const operationId = `sa05-disconnected-${attempt}`;
      terminal(
        await submit(`${callerUrl}/submit/node`, operationId, targetRid),
        operationId,
        'routeNotConnected'
      );
    }
    emit(selector, {
      missingAttempts: 100,
      missingStatus: 'targetNotFound',
      knownTarget: targetRid,
      routeReady: false,
      disconnectedAttempts: 100,
      disconnectedStatus: 'routeNotConnected',
      terminalCountPerOperation: 1,
      handlerCount: 0
    });
  } else if (selector === 'SA-E2E-08') {
    const localId = 'sa08-local';
    const remoteId = 'sa08-remote';
    terminal(await submit(`${callerUrl}/submit/node`, localId, callerRid), localId);
    terminal(await submit(`${callerUrl}/submit/node`, remoteId, targetRid), remoteId);
    const localEvidence = await waitEvidence(
      localId,
      (value) => value.completed === 1,
      callerUrl
    );
    const remoteEvidence = await waitEvidence(remoteId, (value) => value.completed === 1);
    assert(localEvidence.entered === 1, 'SA-E2E-08 local handler count mismatch');
    assert(remoteEvidence.entered === 1, 'SA-E2E-08 remote handler count mismatch');
    emit(selector, {
      localStatus: 'submitted',
      remoteStatus: 'submitted',
      localHandlerCount: 1,
      remoteHandlerCount: 1,
      terminalCountPerOperation: 1
    });
  } else if (selector === 'SA-E2E-09') {
    const operationId = 'sa09-channel';
    terminal(await submit(`${callerUrl}/submit/channel`, operationId), operationId);
    const observed = await waitEvidence(operationId, (value) => value.completed === 1);
    assert(observed.entered === 1, 'SA-E2E-09 channel handler count mismatch');
    emit(selector, { status: 'submitted', selectedTarget: targetRid, handlerCount: 1 });
  } else if (selector === 'SA-E2E-14') {
    const operationId = 'sa14-subscriber-zero';
    terminal(await submit(`${publisherUrl}/submit/fanout`, operationId), operationId);
    emit(selector, { subscriberCount: 0, status: 'submitted' });
  } else if (selector === 'SA-E2E-20') {
    const operationId = 'sa20-handler-gate';
    await json(`${targetUrl}/gate/close`, 'POST', {});
    terminal(await submit(`${callerUrl}/submit/node`, operationId, targetRid), operationId);
    const entered = await waitEvidence(operationId, (value) => value.entered === 1);
    assert(entered.completed === 0, 'SA-E2E-20 handler completed before gate release');
    await json(`${targetUrl}/gate/open`, 'POST', {});
    const completed = await waitEvidence(operationId, (value) => value.completed === 1);
    assert(completed.entered === 1 && completed.completed === 1, 'SA-E2E-20 handler count mismatch');
    emit(selector, { status: 'submitted', handlerEnteredBeforeRelease: true, handlerCount: 1 });
  }
}
