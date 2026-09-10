'use strict';

function waitForExit(child, timeoutMs) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve({ forced: false });
  return new Promise((resolve) => {
    let settled = false;
    const finish = (forced) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      child.removeListener('exit', onExit);
      resolve({ forced });
    };
    const onExit = () => finish(false);
    const timer = setTimeout(() => {
      try { child.kill('SIGKILL'); } finally { finish(true); }
    }, timeoutMs);
    child.once('exit', onExit);
  });
}

async function stopChild(child, timeoutMs = 5_000) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return { forced: false };
  child.kill('SIGTERM');
  return waitForExit(child, timeoutMs);
}

async function closeServer(server, timeoutMs = 5_000) {
  if (!server || !server.listening) return { forced: false };
  let forced = false;
  let timer;
  try {
    await Promise.race([
      new Promise((resolve) => server.close(resolve)),
      new Promise((resolve) => { timer = setTimeout(() => {
      forced = true;
      server.closeAllConnections?.();
      resolve();
      }, timeoutMs); })
    ]);
  } finally {
    clearTimeout(timer);
  }
  return { forced };
}

async function closeBrowser(browser, timeoutMs = 5_000) {
  if (!browser) return { forced: false };
  let timer;
  try {
    let forced = false;
    await Promise.race([
      browser.close(),
      new Promise((resolve) => { timer = setTimeout(() => {
        forced = true;
        browser.process?.()?.kill('SIGKILL');
        resolve();
      }, timeoutMs); })
    ]);
    return { forced };
  } finally {
    clearTimeout(timer);
  }
}

module.exports = { closeBrowser, closeServer, stopChild, waitForExit };
