// Boot the machine the app talks to.
//
//   ?wasm=1  → the Loom runtime runs in-browser: boot it, load the app's
//              modules, route fetch('/api/*') to it, and back useVariable() with
//              a WasmMachine. No server — this is the "ship the app as a website"
//              path.
//   (default) → the real OPC-UA machine talking to a native runtime server.
//
// main.tsx awaits this before first render and hands the result to MachineProvider.
//
// createLoomRuntime/WasmMachine now live in packages/loom-wasm (the
// npm-publishable package other consumers install) -- imported here by
// relative path rather than via node_modules/workspaces, since this repo
// intentionally has no workspace tooling (one new package doesn't justify a
// monorepo migration). This IS loom's own frontend dogfooding that package.
// @ts-expect-error — plain-JS runtime service (see loomRuntime.js)
import { createLoomRuntime } from '../../../packages/loom-wasm/src/loomRuntime.js';
import { WasmMachine } from '../../../packages/loom-wasm/src/WasmMachine';
import { isWasmMode } from './wasmMode';

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('failed to load ' + src));
    document.head.appendChild(s);
  });
}

/** Modules to auto-load in wasm mode — the bundled example modules, same set
 *  native boots from a clean instances.json (modules/CMakeLists.txt's EMSCRIPTEN
 *  list, kept in sync with this one). EtherCAT is the one exception: it needs a
 *  real EtherCAT master (SOEM) + raw-Ethernet access, neither available in a
 *  browser, so it's native-only and not built for wasm at all. */
const WASM_MODULES = [
  'class_based.so',
  'command_probe.so',
  'crasher.so',
  'example_motor.so',
  'oscilloscope.so',
  'pneumatic_actuator.so',
  'sequencer.so',
  'stack_light.so',
];

export async function bootMachine(): Promise<unknown> {
  if (!isWasmMode()) {
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
