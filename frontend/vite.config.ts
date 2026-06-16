import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  // The Loom UI is served under /_loom by the runtime (see server.cpp), so asset
  // URLs and the router base must be prefixed. Keep in sync with BrowserRouter's
  // basename in App.tsx. Dev server therefore also serves at /_loom/.
  base: '/_loom/',
  plugins: [react()],
  server: {
    proxy: {
      // OPC-UA facade push channel (WebSocket) — listed before '/api' and with
      // ws:true so the upgrade is proxied in dev (the plain '/api' entry is HTTP).
      '/api/1.0/pushchannel': {
        target: 'ws://localhost:8080',
        ws: true,
      },
      '/api': 'http://localhost:8080',
      '/ws': {
        target: 'ws://localhost:8080',
        ws: true,
      },
    },
  },
})
