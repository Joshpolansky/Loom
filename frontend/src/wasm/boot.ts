// Boot the machine the app talks to.
//
//   ?wasm=1  → the Loom runtime runs in-browser: boot it, load the app's
//              modules, route fetch('/api/*') to it, and back useVariable() with
//              a WasmMachine. No server — this is the "ship the app as a website"
//              path.
//   (default) → the real OPC-UA machine talking to a native runtime server.
//
// main.tsx awaits this before first render and hands the result to MachineProvider.
// @ts-expect-error — plain-JS runtime service (see loomRuntime.js)
import { createLoomRuntime } from './loomRuntime.js';
import { WasmMachine } from './WasmMachine';

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('failed to load ' + src));
    document.head.appendChild(s);
  });
}

/** Modules to auto-load in wasm mode. For now the bundled demo; later, the
 *  application's own SIDE_MODULE .wasm files (built via loom_add_module). */
const WASM_MODULES = ['demo_module.so'];

export async function bootMachine(): Promise<unknown> {
  const useWasm = new URLSearchParams(location.search).has('wasm');
  if (!useWasm) {
    return (await import('../api/machine')).machine; // native: real OpcuaMachine
  }

  const base = import.meta.env.BASE_URL; // '/_loom/'
  await loadScript(base + 'loom_wasm.js'); // defines global createLoomModule
  const createModule = (window as unknown as { createLoomModule: () => Promise<unknown> }).createLoomModule;

  const rt = await createLoomRuntime({ createModule: () => createModule() });
  for (const name of WASM_MODULES) {
    const bytes = new Uint8Array(await (await fetch(base + name)).arrayBuffer());
    rt.loadModule(name, bytes);
  }
  rt.installFetch(); // from here, fetch('/api/*') is served by the in-browser runtime
  return new WasmMachine(rt);
}
