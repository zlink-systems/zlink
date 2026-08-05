import { defineConfig } from '@playwright/test';

const previewPort = Number(process.env.ZONEWORLD_BROWSER_PREVIEW_PORT ?? '48180');

export default defineConfig({
  testDir: './tests/live',
  timeout: 45_000,
  workers: 1,
  use: { baseURL: `http://127.0.0.1:${previewPort}`, headless: true },
  webServer: {
    command: `npm run build && npm exec vite preview -- --host 127.0.0.1 --port ${previewPort}`,
    port: previewPort,
    reuseExistingServer: false,
  },
});
