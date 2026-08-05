import { bootstrapShoppingMall } from '../bootstrap';

bootstrapShoppingMall('api-b').catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
