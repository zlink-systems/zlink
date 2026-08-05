import { startPublisherHost } from './publisher-host-factory';

startPublisherHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
