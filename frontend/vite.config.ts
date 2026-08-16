import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';

// The whole SPA has to live in a ~640 KiB LittleFS partition together with the
// user's configuration, and every byte is served by a 240 MHz MCU over Wi-Fi.
// So: one JS chunk, one CSS chunk, no source maps in the shipped build, no
// hashed-name explosion. The PlatformIO pre-build hook gzips dist/ into
// firmware/data/www and the firmware serves the .gz files directly.
export default defineConfig({
  plugins: [svelte()],
  build: {
    target: 'es2020',
    cssCodeSplit: false,
    sourcemap: false,
    reportCompressedSize: true,
    chunkSizeWarningLimit: 200,
    rollupOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name][extname]',
        manualChunks: undefined,
      },
    },
  },
  server: {
    // `npm run dev` against a real board: point this at its address.
    proxy: {
      '/api': { target: 'http://lab-controller.local', changeOrigin: true },
      '/ws': { target: 'ws://lab-controller.local', ws: true },
    },
  },
});
