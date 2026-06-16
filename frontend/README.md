# Loom Frontend

The React + TypeScript (Vite) debug IDE for the [Loom](../README.md) runtime.

## Data layer

Live data flows over the runtime's mapp Connect / OPC-UA facade (`/api/1.0`) via
[`@loupeteam/lux-connect`](https://www.npmjs.com/package/@loupeteam/lux-connect) +
[`@loupeteam/lux-react`](https://www.npmjs.com/package/@loupeteam/lux-react):

- `src/api/machine.ts` — one `OpcuaMachine` pointed at the same origin, plus the
  `node()` / `classNode()` NodeId helpers (`ns=1;s=/module/<id>/<section>[/field]`).
- `App.tsx` wraps the tree in `<MachineProvider>`; the connection indicator reads
  `useMachine().connectionState`.
- Components read live values with `useVariable(node(...))` (whole-section
  subscriptions for the tree views; per-leaf in WatchView) and write with
  `useMachine().writeVariable`.
- `src/api/dataService.ts` is now just a REST metadata store (module list +
  per-module detail/schema). Everything without an OPC-UA equivalent — history
  charts, config/recipe save/load, instantiate/reload, bus, oscilloscope, IO
  mappings — stays on the plain `/api/*` REST helpers in `src/api/rest.ts`.

## Develop

```bash
npm install        # once
npm run dev        # Vite on :5173, proxies /api + /api/1.0 to the runtime on :8080
npm run build      # emits dist/ — `just frontend` / `just run` serve this
```

Run the Loom runtime (`just run`) alongside `npm run dev` so the facade is available.
