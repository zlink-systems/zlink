import { startSubscriberHost } from './subscriber-host-factory';

startSubscriberHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
