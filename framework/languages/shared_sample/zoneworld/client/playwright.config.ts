import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests/e2e',
  use: { baseURL: 'http://127.0.0.1:48180', headless: true },
  webServer: {
    command: 'npm run build && npm exec vite preview -- --host 127.0.0.1 --port 48180',
    port: 48180,
    reuseExistingServer: false,
  },
});
