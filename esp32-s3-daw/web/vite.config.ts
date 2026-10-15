// SPDX-License-Identifier: GPL-3.0-or-later
// The device serves dist/ from flash (gzip-compressed by scripts/embed-web.py): one small JS bundle,
// one CSS file, no CDN, no source maps. `npm run dev` proxies the API to a running daw-host.
import { defineConfig } from 'vite';

export default defineConfig({
  esbuild: { jsx: 'automatic', jsxImportSource: 'preact', legalComments: 'none' },
  build: {
    target: 'es2019',
    outDir: 'dist',
    assetsDir: 'assets',
    sourcemap: false,
    cssCodeSplit: false,
    modulePreload: false,
    reportCompressedSize: true,
    rollupOptions: { output: { manualChunks: undefined } },
  },
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8080',
      '/ws': { target: 'ws://127.0.0.1:8080', ws: true },
    },
  },
});
