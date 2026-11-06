function nodeTestConcurrencyArgs() {
  return process.env.ZLINK_SINGLE_WORKER === 'true' ? ['--test-concurrency=1'] : [];
}

module.exports = { nodeTestConcurrencyArgs };
