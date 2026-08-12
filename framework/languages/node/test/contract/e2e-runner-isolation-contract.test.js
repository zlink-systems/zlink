const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '../..');
const e2eRoot = path.join(workspaceRoot, 'e2e');

function read(relativePath) {
  return fs.readFileSync(path.join(e2eRoot, relativePath), 'utf8');
}

function standaloneRunners() {
  return fs.readdirSync(e2eRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => path.join(e2eRoot, entry.name, 'run_e2e.sh'))
    .filter((file) => fs.existsSync(file))
    .sort();
}

test('Node E2E runners serialize whole runs without locking the sequential aggregate', () => {
  const runners = standaloneRunners();
  assert.equal(runners.length, 14);

  for (const runner of runners) {
    const source = fs.readFileSync(runner, 'utf8');
    assert.match(source, /source "\$NODE_ROOT\/e2e\/runner-common\.sh"/,
      `${runner} must use the shared language lock`);
    assert.match(source, /serialize_node_e2e_run "\$0" "\$@"/,
      `${runner} must serialize the complete standalone run`);
    const lockOffset = source.indexOf('serialize_node_e2e_run');
    for (const resourceMarker of [
      'RUN_ID=', 'run_id=', 'mktemp', 'allocate_port', 'pick_port',
      'start_redis_container', 'docker create', 'docker start'
    ]) {
      const resourceOffset = source.indexOf(resourceMarker);
      assert.ok(resourceOffset < 0 || lockOffset < resourceOffset,
        `${runner} must acquire the lock before ${resourceMarker}`);
    }
    const cleanupTrapOffset = source.indexOf('trap cleanup EXIT');
    const logDirectoryOffset = source.indexOf('mkdir -p "$LOG_DIR"');
    assert.ok(cleanupTrapOffset >= 0 && logDirectoryOffset > cleanupTrapOffset,
      `${runner} must install cleanup before creating its run log directory`);
    assert.doesNotMatch(source, /docker rm -fv/,
      `${runner} must delegate exact-ID Redis cleanup to the shared helper`);
    for (const line of source.split('\n')) {
      if (!/\bdocker\s+(?:pause|unpause|stop|start|inspect|exec)\b/.test(line)
          || /redis-cli --csv monitor/.test(line)) continue;
      assert.match(line, /timeout /,
        `${runner} has an unbounded short Docker control command: ${line.trim()}`);
    }
  }

  const common = read('runner-common.sh');
  assert.match(common,
    /NODE_E2E_LANGUAGE_LOCK_FILE="\/tmp\/zlink-framework-node-e2e\.lock"/);
  assert.doesNotMatch(common, /TMPDIR/);
  assert.match(common, /ZLINK_NODE_E2E_LANGUAGE_LOCK_HELD/);
  assert.match(common, /exec flock --exclusive --close/);
  assert.match(common, /remove_redis_container_by_id/);
  assert.match(common, /\[\[ "\$container_id" =~ \^\[0-9a-f\]\{12,64\}\$ \]\] \|\| return 1/);
  assert.match(common,
    /timeout -k 2s "\$\{docker_timeout_seconds\}s" docker rm -fv "\$container_id"/);

  const aggregate = read('run_e2e_all.sh');
  assert.doesNotMatch(aggregate, /serialize_node_e2e_run|NODE_E2E_LANGUAGE_LOCK_FILE/);
  assert.match(aggregate, /timeout "\$\{SCENARIO_TIMEOUT_SECONDS\}s" bash \.\/run_e2e\.sh/);
  assert.doesNotMatch(aggregate, /\[\[ ! -x .*run_e2e\.sh/);
  assert.match(aggregate, /for index in "\$\{!selected_configs\[@\]\}"/);
  assert.match(aggregate, /wait "\$\{active_config_pid\}"/);
  const childLaunchOffset = aggregate.indexOf('active_config_pid="$!"');
  assert.ok(childLaunchOffset >= 0);
  assert.ok(childLaunchOffset
    < aggregate.indexOf('wait "${active_config_pid}"', childLaunchOffset));
  assert.match(aggregate,
    /run_config_with_retry "\$\{config\}" "\$\{scenario\}"/);
  assert.doesNotMatch(aggregate,
    /run_config_with_retry "\$\{config\}" "\$\{scenario\}"\s*&/);

  const spotActorTransfer = read('SpotActorTransfer/run_e2e.sh');
  assert.match(spotActorTransfer,
    /bash "\$ROOT_DIR\/run_e2e\.sh" "\$child_scenario" --child-run/);

  for (const scenario of [
    'ResilienceLifecycle/Client/Scenarios/rl-c4-store-outage-scenario.ts',
    'RuntimeMonitoring/Client/Scenarios/mon-a5-fixed-kinds-scenario.ts'
  ]) {
    const source = read(scenario);
    assert.match(source, /spawn\('docker',[\s\S]*?timeout: 10_000,[\s\S]*?killSignal: 'SIGKILL'/,
      `${scenario} must bound short Docker fault-control commands`);
  }
});

test('Node E2E Redis and application listeners use disjoint checked port pools', () => {
  const common = read('runner-common.sh');
  const picker = read('port-picker.js');
  const redis = read('redis-container.sh');

  assert.match(common, /NODE_E2E_APPLICATION_PORT_MIN=38100/);
  assert.match(common, /NODE_E2E_APPLICATION_PORT_MAX=39999/);
  assert.match(common, /allocated-ports/);
  assert.match(picker, /const MIN_PORT = 38100/);
  assert.match(picker, /const MAX_PORT = 39999/);
  assert.match(picker, /server\.listen\(port, '127\.0\.0\.1'/);

  assert.match(redis, /NODE_E2E_REDIS_PORT_MIN=38000/);
  assert.match(redis, /NODE_E2E_REDIS_PORT_MAX=38099/);
  assert.match(redis, /sock\.bind\(\("127\.0\.0\.1", candidate\)\)/);
  assert.match(redis, /-p "127\.0\.0\.1:\$\{selected_port\}:6379"/);
  assert.match(redis, /"\$published_port" == "\$selected_port"/);
  assert.match(redis, /remove_redis_attempt "\$candidate" "\$attempt_name"/);
  assert.match(redis, /docker inspect --type container/);
  assert.match(redis, /create_output.*port is already allocated|port is already allocated.*create_output/s);
  assert.doesNotMatch(redis, /127\.0\.0\.1::6379/);

  for (const runner of standaloneRunners()) {
    const source = fs.readFileSync(runner, 'utf8');
    assert.doesNotMatch(source, /127\.0\.0\.1::6379|random\.randint\(20000, 60999\)/,
      `${runner} must not allocate outside the Node E2E pools`);
    assert.doesNotMatch(source, /ZLINK_TEST_REDIS_ENDPOINT/,
      `${runner} must not borrow an external Redis endpoint`);
  }
});
