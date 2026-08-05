import 'reflect-metadata';
import { startConsumerHost } from './consumer-host-factory';

startConsumerHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
