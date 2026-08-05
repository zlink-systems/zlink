import 'reflect-metadata';
import { startObjectClientHost } from './object-client-host-factory';

startObjectClientHost().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
