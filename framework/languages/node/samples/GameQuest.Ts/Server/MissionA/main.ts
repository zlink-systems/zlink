import { bootstrapGameQuest } from '../bootstrap';

bootstrapGameQuest('mission-a').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
