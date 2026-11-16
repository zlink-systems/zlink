const childProcess = require('node:child_process');

//  `node --test` runs each file in an isolated child. A file that holds its
//  event loop (#4052) cannot run its SIGTERM handler, so signalling only the
//  `node --test` process never ends it and the gate waits until the job limit.
//  The deadline therefore kills the whole process tree and names the test that
//  was running. Output is forwarded as it arrives so the log keeps the TAP up
//  to the stall.
const activeChildren = new Set();
process.once('exit', () => {
  for (const child of activeChildren) killProcessTree(child);
});

function runTestFileWithDeadline(args, { cwd, env, deadlineMs }) {
  return new Promise((resolve) => {
    const child = childProcess.spawn(process.execPath, args, {
      cwd,
      env,
      stdio: ['ignore', 'pipe', 'pipe'],
      //  A separate process group lets one kill reach the isolated test child.
      detached: process.platform !== 'win32'
    });
    activeChildren.add(child);
    let stdout = '';
    let partialLine = '';
    //  TAP opens a subtest with `# Subtest:` and closes it with `ok`/`not ok` at
    //  the same indent, so the open subtests are the path of the test that stalled.
    const openSubtests = [];
    let lastOk;
    let error;
    let stalled = false;
    let processes = [];
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
      process.stdout.write(chunk);
      const lines = (partialLine + chunk).split(/\r?\n/);
      partialLine = lines.pop();
      for (const line of lines) track(line);
    });
    child.stderr.on('data', (chunk) => process.stderr.write(chunk));
    child.once('error', (spawnError) => {
      error = spawnError;
    });
    const deadline = setTimeout(() => {
      stalled = true;
      //  Which processes still hold the group tells whether `node --test` or the
      //  isolated test child stalled.
      processes = describeProcessGroup(child);
      killProcessTree(child);
    }, deadlineMs);
    child.once('close', (code, signal) => {
      clearTimeout(deadline);
      activeChildren.delete(child);
      track(partialLine);
      resolve({
        status: code ?? 1,
        signal,
        stdout,
        error,
        stalled,
        running: openSubtests.map((subtest) => subtest.name).join(' > '),
        lastOk,
        processes
      });
    });

    function track(line) {
      const started = /^(\s*)# Subtest: (.*)$/.exec(line);
      if (started !== null) openSubtests.push({ indent: started[1].length, name: started[2] });
      const finished = /^(\s*)(not )?ok \d+ - (.*)$/.exec(line);
      if (finished === null) return;
      const indent = finished[1].length;
      while (openSubtests.length > 0 && openSubtests.at(-1).indent >= indent) openSubtests.pop();
      if (finished[2] === undefined) lastOk = finished[3];
    }
  });
}

//  The group is killed even after `node --test` itself exited: an orphaned test
//  child that still holds the output pipe keeps the file from closing.
function killProcessTree(child) {
  if (child.pid === undefined) return;
  try {
    if (process.platform === 'win32') {
      childProcess.spawnSync('taskkill', ['/pid', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
    } else {
      process.kill(-child.pid, 'SIGKILL');
    }
  } catch {
    //  The whole group already exited.
  }
}

function describeProcessGroup(child) {
  if (child.pid === undefined || process.platform === 'win32') return [];
  const listing = childProcess.spawnSync('ps', ['-eo', 'pid=,pgid=,args='], { encoding: 'utf8' });
  return (listing.stdout ?? '')
    .split(/\r?\n/)
    .map((line) => /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(line))
    .filter((match) => match !== null && Number(match[2]) === child.pid)
    .map((match) => `${match[1]} ${match[3]}`);
}

module.exports = { runTestFileWithDeadline };
