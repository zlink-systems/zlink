import type { INestApplicationContext } from '@nestjs/common';

function waitForShutdown(): Promise<void> {
  return new Promise((resolve) => {
    const stop = () => resolve();
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

async function closeRuntime(app: INestApplicationContext): Promise<void> {
  await app.close();
}

export { closeRuntime, waitForShutdown };
