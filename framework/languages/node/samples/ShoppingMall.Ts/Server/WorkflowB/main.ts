import { bootstrapShoppingMall } from '../bootstrap';

bootstrapShoppingMall('workflow-b').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
