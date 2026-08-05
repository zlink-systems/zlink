import 'reflect-metadata';
import { startProviderHost } from './provider-host-factory';

startProviderHost().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
