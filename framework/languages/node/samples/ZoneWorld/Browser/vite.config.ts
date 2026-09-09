import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

export default defineConfig({
  plugins: [preact()],
  build: {
    rollupOptions: {
      input: {
        game: new URL('./game.html', import.meta.url).pathname,
        ops: new URL('./ops.html', import.meta.url).pathname,
      },
    },
  },
});
