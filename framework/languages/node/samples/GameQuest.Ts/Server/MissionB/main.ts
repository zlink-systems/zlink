import { bootstrapGameQuest } from '../bootstrap';

bootstrapGameQuest('mission-b').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
