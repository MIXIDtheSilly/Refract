import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// `npm run dev` / `npm run build` serve the Tauri window. `npm run preview:build` makes
// one self-contained HTML file with sample data that opens in any browser.
export default defineConfig(({ mode }) => ({
  base: './',
  plugins: [react(), ...(mode === 'preview' ? [viteSingleFile()] : [])],
  clearScreen: false,
  server: { port: 5173, strictPort: true },
  build: {
    outDir: mode === 'preview' ? 'dist-preview' : 'dist',
    emptyOutDir: true,
    target: 'chrome120',
    assetsInlineLimit: mode === 'preview' ? 100_000_000 : 4096,
  },
}));
