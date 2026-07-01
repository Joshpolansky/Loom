// The package's main entry point: boot the Loom runtime in-browser from a
// real dataDir/moduleDir (the same manifest-driven boot native's
// `loom --module-dir --data-dir` uses -- see bootFromDataDir in
// loomRuntime.js) and return a WasmMachine, a drop-in ICommLayer for
// @loupeteam/lux-react's <MachineProvider machine={...}>. An app that
// already uses lux-react's useVariable()/useMachine() hooks needs zero
// changes anywhere else to consume it.
import { bootFromDataDir } from './loomRuntime.js';
import { WasmMachine } from './WasmMachine.js';

export interface BootWasmMachineOptions {
  /** Base URL serving the dataDir's contents (instances.json, scheduler.json,
   *  <id>/config.json, ...) read-only -- e.g. '/data/loom'. */
  dataUrl: string;
  /** Base URL serving the moduleDir's .so files -- e.g. '/output/modules'. */
  moduleUrl: string;
  /** Base URL serving loom_wasm.js/.wasm (this package ships them under its
   *  own wasm/ dir -- copy that into your app's static output and point
   *  this at wherever it ends up served, e.g. '/loom-wasm'). Required and
   *  not defaulted: resolving it relative to the package's own install
   *  location depends on the consumer's bundler, which this package
   *  deliberately doesn't assume. */
  wasmUrl: string;
  /** Cooperative tick cadence, ms. Default 25. */
  tickMs?: number;
  /** Poll interval WasmMachine uses for subscribe()-backed variables, ms. Default 100. */
  pollMs?: number;
}

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('failed to load ' + src));
    document.head.appendChild(s);
  });
}

export async function bootWasmMachine(opts: BootWasmMachineOptions): Promise<WasmMachine> {
  const { dataUrl, moduleUrl, wasmUrl, tickMs, pollMs } = opts;
  const base = wasmUrl.endsWith('/') ? wasmUrl : wasmUrl + '/';

  await loadScript(base + 'loom_wasm.js');
  const createModule = (window as unknown as { createLoomModule?: () => Promise<unknown> }).createLoomModule;
  if (!createModule) {
    throw new Error(`bootWasmMachine: ${base}loom_wasm.js did not define window.createLoomModule`);
  }

  const rt = await bootFromDataDir({ createModule: () => createModule(), dataUrl, moduleUrl, tickMs });
  if (rt.skipped.length) {
    // Expected for any instance not built for wasm (e.g. real-hardware-only
    // modules) -- the C++ loader also refuses a version/ABI-mismatched .so
    // (see runtime/src/module_loader.cpp) and bootFromDataDir folds that
    // case into `skipped` too, distinct from a raw 404.
    console.warn(`bootWasmMachine: ${rt.skipped.length} instance(s) not loaded, skipped:`, rt.skipped);
  }
  return new WasmMachine(rt, pollMs);
}
