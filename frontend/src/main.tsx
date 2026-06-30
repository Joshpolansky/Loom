import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.tsx'
import { bootMachine } from './wasm/boot'

// Boot the machine first: a WasmMachine (in-browser runtime, ?wasm=1) or the real
// OPC-UA machine. Render once it's ready so the providers get a live machine.
bootMachine().then((machine) => {
  createRoot(document.getElementById('root')!).render(
    <StrictMode>
      <App machine={machine} />
    </StrictMode>,
  )
})
