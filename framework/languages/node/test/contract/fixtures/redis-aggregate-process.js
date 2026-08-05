const {
  ZLinkRedisLocationStore
} = require('../../../packages/framework-locations-redis/dist');
const {
  ZLinkLocationStoreRepository
} = require('../../../packages/framework/dist/internal');

async function main() {
  const [operation, url, keyPrefix, aggregateId, generationText] =
    process.argv.slice(2);
  if (
    !['commit', 'commit-crash', 'abort', 'abort-crash'].includes(operation)
    || !url
    || !keyPrefix
    || !aggregateId
    || !generationText
  ) {
    throw new Error(
      'usage: redis-aggregate-process.js <commit|commit-crash|abort|abort-crash> <url> <prefix> <id> <generation>'
    );
  }
  const redis = new ZLinkRedisLocationStore({ url, keyPrefix });
  const provider = operation.endsWith('-crash')
    ? new ExitAfterAggregatePublicationStore(
        redis,
        operation === 'commit-crash' ? 'committed' : 'aborted',
        operation === 'commit-crash' ? 91 : 92
      )
    : redis;
  try {
    const repository = new ZLinkLocationStoreRepository(provider);
    const fence = {
      aggregateId: { value: aggregateId },
      aggregateGeneration: BigInt(generationText)
    };
    const result = operation.startsWith('commit')
      ? await repository.commitAggregate(fence)
      : await repository.abortAggregate(fence);
    process.stdout.write(`${JSON.stringify(result)}\n`);
  } finally {
    await provider.dispose();
  }
}

class ExitAfterAggregatePublicationStore {
  constructor(inner, terminalState, exitCode) {
    this.inner = inner;
    this.terminalState = terminalState;
    this.exitCode = exitCode;
  }

  read(key, signal) {
    return this.inner.read(key, signal);
  }

  scan(request, signal) {
    return this.inner.scan(request, signal);
  }

  async write(request, signal) {
    const publishesAggregate = request.mutations.some(mutation =>
      mutation.kind === 'put'
      && mutation.key.value.startsWith('zlink:v11:aggregate:')
      && Buffer.from(mutation.bytes).toString('utf8')
        .includes(`"state":"${this.terminalState}"`));
    const result = await this.inner.write(request, signal);
    if (publishesAggregate && result.kind === 'applied') {
      process.exit(this.exitCode);
    }
    return result;
  }

  dispose() {
    return this.inner.dispose();
  }
}

main().catch(error => {
  process.stderr.write(`${error?.stack ?? error}\n`);
  process.exitCode = 1;
});
