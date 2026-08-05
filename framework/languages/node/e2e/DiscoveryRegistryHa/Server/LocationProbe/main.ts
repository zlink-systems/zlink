import 'reflect-metadata';
import { startLocationProbeHost } from './location-probe-host-factory';

startLocationProbeHost().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
