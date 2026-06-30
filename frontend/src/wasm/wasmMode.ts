// Single source of truth for "is the app driving an in-browser wasm runtime?"
//
//  - Production: the website is BUILT in wasm mode (VITE_WASM=1 at build time),
//    so there is no server and this is always true. This is the "ship the app as
//    a website" path.
//  - Dev: pass ?wasm=1 once. It's persisted in sessionStorage so SPA navigation
//    (which drops the query string) doesn't silently fall back to native mode.
//    Pass ?no-wasm to clear it.
export function isWasmMode(): boolean {
  const built = (import.meta.env as Record<string, unknown>).VITE_WASM;
  if (built === '1' || built === 'true') return true;

  try {
    const params = new URLSearchParams(window.location.search);
    if (params.has('no-wasm')) sessionStorage.removeItem('loom.wasm');
    else if (params.has('wasm')) sessionStorage.setItem('loom.wasm', '1');
    return sessionStorage.getItem('loom.wasm') === '1';
  } catch {
    return false;
  }
}
