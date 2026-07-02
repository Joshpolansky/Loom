import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.tsx'
import { bootMachine } from './wasm/boot'

// Boot the machine first: a WasmMachine (in-browser runtime, ?wasm=1) or the real
// OPC-UA machine. Render once it's ready so the providers get a live machine.
// A boot failure must render SOMETHING — without the catch it's an unhandled
// rejection and a permanently blank page.
bootMachine().then((machine) => {
  createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <App machine={machine} />
    </StrictMode>,
  )
}).catch((err: unknown) => {
  console.error('bootMachine failed:', err)
  const root = document.getElementById('root')
  if (root) {
    root.innerHTML =
      '<div style="font-family: system-ui, sans-serif; padding: 2rem; color: #ff6b6b">' +
      '<h2>Loom failed to start</h2><pre style="white-space:pre-wrap"></pre></div>'
    root.querySelector('pre')!.textContent = String(err)
  }
})
