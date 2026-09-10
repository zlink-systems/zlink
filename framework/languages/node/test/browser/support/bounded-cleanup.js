'use strict';

function waitForExit(child, timeoutMs) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve({ timedOut: false, forced: false });
  return new Promise((resolve) => {
    let settled = false;
    const finish = (forced) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      child.removeListener('exit', onExit);
      resolve({ timedOut: forced, forced });
    };
    const onExit = () => finish(false);
    const timer = setTimeout(() => {
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
    if (timedOut) server.kill();
  } finally { clearTimeout(timer); }
  return { timedOut, forced: timedOut };
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

module.exports = { closeBrowser, closeBrowserServer, closeContext, closeServer, stopChild, waitForExit };
