'use strict';

function waitForExit(child, timeoutMs) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve({ timedOut: false, forced: false });
  return new Promise((resolve) => {
    let settled = false;
    let forceIssued = false;
    const finish = (forced) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      child.removeListener('exit', onExit);
      resolve({ timedOut: forced || forceIssued, forced: forced || forceIssued });
    };
    const onExit = () => finish(false);
    const timer = setTimeout(() => {
      forceIssued = true;
      try { child.kill('SIGKILL'); } finally { finish(true); }
    }, timeoutMs);
    child.once('exit', onExit);
  });
}

async function stopChild(child, timeoutMs = 5_000) {
  if (!child) return { timedOut: false, forced: false };
  const alreadyExited = child.exitCode !== null || child.signalCode !== null;
  const result = alreadyExited ? { timedOut: false, forced: false } : (child.kill('SIGTERM'), await waitForExit(child, timeoutMs));
  child.stdout?.destroy();
  child.stderr?.destroy();
  return result;
}

async function stopChildGracefully(child, requestStop, gracefulTimeoutMs = 10_000, forceTimeoutMs = 5_000) {
  if (!child || child.exitCode !== null || child.signalCode !== null) {
    child?.stdout?.destroy(); child?.stderr?.destroy();
    return { timedOut: false, forced: false };
  }
  await requestStop();
  let timer;
  const graceful = await Promise.race([
    new Promise((resolve) => child.once('exit', () => resolve(true))),
    new Promise((resolve) => { timer = setTimeout(() => resolve(false), gracefulTimeoutMs); })
  ]);
  clearTimeout(timer);
  if (graceful) {
    child.stdout?.destroy(); child.stderr?.destroy();
    return { timedOut: false, forced: false };
  }
  return stopChild(child, forceTimeoutMs);
}

async function closeContext(context, timeoutMs = 5_000) {
  if (!context) return { timedOut: false, forced: false };
  let timedOut = false;
  let timer;
  try {
    await Promise.race([
      context.close(),
      new Promise((resolve) => { timer = setTimeout(() => { timedOut = true; resolve(); }, timeoutMs); })
    ]);
  } finally { clearTimeout(timer); }
  return { timedOut, forced: false };
}

async function closeBrowserServer(server, timeoutMs = 5_000) {
  if (!server) return { timedOut: false, forced: false };
  let timedOut = false;
  let timer;
  try {
    await Promise.race([
      server.close(),
      new Promise((resolve) => { timer = setTimeout(() => { timedOut = true; resolve(); }, timeoutMs); })
    ]);
    if (timedOut) {
      let killTimedOut = false;
      let killTimer;
      try {
        await Promise.race([
          Promise.resolve(server.kill()),
          new Promise((resolve) => { killTimer = setTimeout(() => { killTimedOut = true; resolve(); }, timeoutMs); })
        ]);
      } finally { clearTimeout(killTimer); }
      return { timedOut: true, forced: !killTimedOut, killTimedOut };
    }
  } finally { clearTimeout(timer); }
  return { timedOut, forced: false };
}

async function closeServer(server, timeoutMs = 5_000) {
  if (!server || !server.listening) return { timedOut: false, forced: false };
  let timedOut = false;
  let timer;
  try {
    await Promise.race([
      new Promise((resolve) => server.close(resolve)),
      new Promise((resolve) => { timer = setTimeout(() => {
        timedOut = true;
        server.unref?.();
        server.closeAllConnections?.();
        server.closeIdleConnections?.();
        resolve();
      }, timeoutMs); })
    ]);
  } finally {
    clearTimeout(timer);
  }
  return { timedOut, forced: timedOut };
}

async function closeBrowser(browser, timeoutMs = 5_000) {
  if (!browser) return { timedOut: false, forced: false };
  let timer;
  let timedOut = false;
  try {
    await Promise.race([
      browser.close(),
      new Promise((resolve) => { timer = setTimeout(() => {
        timedOut = true;
        resolve();
      }, timeoutMs); })
    ]);
  } finally {
    clearTimeout(timer);
  }
  return { timedOut, forced: false };
}

module.exports = { closeBrowser, closeBrowserServer, closeContext, closeServer, stopChild, stopChildGracefully, waitForExit };
