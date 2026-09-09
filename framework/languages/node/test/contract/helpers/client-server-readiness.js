async function waitForClientServerTargets(runtime, channelName, expectedCount) {
  while (runtime.snapshot(channelName).readyTargetCount !== expectedCount) {
    await new Promise(resolve => setImmediate(resolve));
  }
}

module.exports = { waitForClientServerTargets };
