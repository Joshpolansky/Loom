import { useEffect, useLayoutEffect, useRef, useState, useCallback } from 'react';
import type { DragEvent } from 'react';
import { getModuleData } from '../api/rest';
import { useDataService } from '../api/dataService';
// DataTree used via SectionPanel
import { DataTree } from '../components/DataTree';
import './WatchView.css';

type WatchEntry = { id: string; moduleId: string; path: string; label: string; expand?: boolean };

const LS_KEY = 'cruntime:watch:list';

function makeId() { return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2,8)}`; }

function loadSaved(): WatchEntry[] {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw) as unknown;
    if (!Array.isArray(parsed)) return [];
    return parsed as WatchEntry[];
  } catch { return []; } // ignore parse errors
}

function saveList(list: WatchEntry[]) { localStorage.setItem(LS_KEY, JSON.stringify(list)); }

export default function WatchView() {
  
  const [showPicker, setShowPicker] = useState(false);
  const [watchList, setWatchList] = useState<WatchEntry[]>(() => loadSaved());
  const [values, setValues] = useState<Record<string, { value: unknown; error?: string; ts?: number }>>({});
  const wsRef = useRef<WebSocket | null>(null);
  const subscribedRef = useRef<Set<string>>(new Set());
  const dragIdRef = useRef<string | null>(null);
  const watchListRef = useRef(watchList);
  const { writeField, modules } = useDataService();

  useLayoutEffect(() => { watchListRef.current = watchList; });

  // no-op: module tree picker will fetch module data on demand

  // Connect to /ws/watch and manage subscriptions.
  useEffect(() => {
    let reconnectTimer: number | null = null;
    // Permissive UTF-8 decoder for binary frames from the server.
    const decoder = new TextDecoder('utf-8', { fatal: false });
    function connect() {
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const ws = new WebSocket(`${proto}//${window.location.host}/ws/watch`);
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => {
        // subscribe all
        for (const w of watchListRef.current) {
          ws.send(JSON.stringify({ type: 'subscribe', id: w.id, moduleId: w.moduleId, path: w.path, expand: !!w.expand }));
          subscribedRef.current.add(w.id);
        }
      };
      ws.onclose = () => {
        subscribedRef.current.clear();
        reconnectTimer = window.setTimeout(connect, 2000);
      };
      ws.onerror = () => ws.close();
      ws.onmessage = (ev) => {
        try {
          const text = typeof ev.data === 'string'
            ? ev.data
            : decoder.decode(ev.data as ArrayBuffer);
          const msg = JSON.parse(text);
          if (msg.type === 'watch' && Array.isArray(msg.values)) {
            setValues(prev => {
              const next = { ...prev };
              for (const it of msg.values) {
                next[it.id] = { value: it.value, error: it.error ?? undefined, ts: msg.ts };
              }
              return next;
            });
          }
        } catch {
          // ignore
        }
      };
      wsRef.current = ws;
    }

    connect();
    return () => {
      if (reconnectTimer) clearTimeout(reconnectTimer);
      wsRef.current?.close();
    };
  }, []); // only once

  // Sync subscriptions when watchList changes
  useEffect(() => {
    saveList(watchList);
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const want = new Set(watchList.map(w => w.id));
    // subscribe new
    for (const w of watchList) {
      if (!subscribedRef.current.has(w.id)) {
        ws.send(JSON.stringify({ type: 'subscribe', id: w.id, moduleId: w.moduleId, path: w.path, expand: !!w.expand }));
        subscribedRef.current.add(w.id);
      }
    }
    // unsubscribe removed
    for (const id of Array.from(subscribedRef.current)) {
      if (!want.has(id)) {
        ws.send(JSON.stringify({ type: 'unsubscribe', id }));
        subscribedRef.current.delete(id);
      }
    }
  }, [watchList]);

  const addEntries = useCallback((selected: WatchEntry[]) => {
    const next = [...watchList];
    for (const s of selected) {
      const id = s.id ?? makeId();
      next.push({ id, moduleId: s.moduleId, path: s.path, label: s.label ?? `${s.moduleId} ${s.path}`, expand: s.expand ?? false });
    }
    setWatchList(next);
    setShowPicker(false);
  }, [watchList]);

  const removeEntry = useCallback((id: string) => {
    setWatchList(prev => prev.filter(p => p.id !== id));
    setValues(prev => { const n = { ...prev }; delete n[id]; return n; });
    // unsubscribe immediately if WS open
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: 'unsubscribe', id }));
    subscribedRef.current.delete(id);
  }, []);

  const reorder = useCallback((fromId: string, toId: string | null) => {
    setWatchList(prev => {
      const next = [...prev];
      const fromIdx = next.findIndex(it => it.id === fromId);
      if (fromIdx === -1) return prev;
      const [item] = next.splice(fromIdx, 1);
      if (!toId) {
        next.push(item);
      } else {
        const toIdx = next.findIndex(it => it.id === toId);
        if (toIdx === -1) next.push(item);
        else next.splice(toIdx, 0, item);
      }
      return next;
    });
  }, []);

  function onDragStart(e: DragEvent, id: string) {
    dragIdRef.current = id;
    try { e.dataTransfer?.setData('text/plain', id); } catch { /* ignore */ }
    e.dataTransfer!.effectAllowed = 'move';
  }
  function onDragOver(e: DragEvent) { e.preventDefault(); e.dataTransfer!.dropEffect = 'move'; }
  function onDrop(e: DragEvent, targetId: string | null) {
    e.preventDefault();
    const from = dragIdRef.current ?? (e.dataTransfer?.getData('text/plain') || null);
    if (!from) return;
    reorder(from, targetId);
    dragIdRef.current = null;
  }

  // (renderValue removed — using SectionPanel/DataTree for rendering now)

  return (
    <div className="watch-page">
      <div className="watch-header">
        <h2>Watch</h2>
        <div>
          <button onClick={() => setShowPicker(true)} className="watch-add-button">Add</button>
        </div>
      </div>
      <div className="watch-list">
        {watchList.length === 0 && <div className="watch-empty">No watched variables — click Add.</div>}
        {watchList.map((w,) => {
          const liveVal = values[w.id]?.value;
          // fallback to module snapshot if live value not present
          const moduleSnapshot = modules[w.moduleId]?.detail?.data?.runtime ?? modules[w.moduleId]?.liveRuntime ?? {};
          let pathSegs = [w.moduleId, ...(w.path || '').split('/').filter(Boolean)];
          const subtree = liveVal !== undefined ? liveVal : (pathSegs.length ? pathSegs.reduce((o: unknown, k: string) => (o && typeof o === 'object' ? (o as Record<string, unknown>)[k] : undefined), moduleSnapshot) : moduleSnapshot);
          const isObject = subtree !== null && typeof subtree === 'object';
          const wrapperKey = !isObject ? (pathSegs.length ? pathSegs[pathSegs.length - 1] : w.moduleId) : null;
          const data = isObject ? (subtree as Record<string, unknown>) : { [wrapperKey as string]: subtree };
          const panelData: Record<string, unknown> = {};
          if(!isObject) pathSegs = pathSegs.slice(0, -1); // if scalar, remove last segment for cleaner display
          const key = pathSegs.join('/');
          panelData[key] = data;

          function handleWrite(localPath: string[], value: unknown) {
            const relativePath = localPath.slice(1); // if wrapperKey, remove it from path before sending to server
            writeField(w.moduleId, 'runtime', relativePath, value);
          }

          return (
            <div
              key={w.id}
              className="watch-row"
              draggable
              onDragStart={(e) => onDragStart(e, w.id)}
              onDragOver={(e) => onDragOver(e)}
              onDrop={(e) => onDrop(e, w.id)}
              onDragEnd={() => { dragIdRef.current = null; }}
            >
              <div className="watch-root-header">
                <div className="watch-meta">
                  <span className="drag-handle">≡</span>
                </div>
                <DataTree 
                    data={panelData}
                    onChange={handleWrite}
                />
                <div className="watch-actions">
                  <button className="watch-remove-button" onClick={() => removeEntry(w.id)}>✕</button>
                </div>
              </div>
            </div>
          );
        })}
      </div>

      {showPicker && (
        <PickerModal
          modules={modules}
          onConfirm={(sel) => addEntries(sel)}
          onCancel={() => setShowPicker(false)}
        />
      )}
    </div>
  );
}

// Scalar editor removed — SectionPanel/DataTree handles editing

// ---------- Picker modal: select modules and whole structures ----------
function PickerModal({ modules, onConfirm, onCancel }: { modules: Record<string, unknown>; onConfirm: (sel: WatchEntry[]) => void; onCancel: () => void }) {
  const moduleIds = Object.keys(modules ?? {}).sort();
  const [selectedModule, setSelectedModule] = useState<string | null>(moduleIds.length ? moduleIds[0] : null);
  const [moduleData, setModuleData] = useState<Record<string, unknown> | null>(null);
  const [loading, setLoading] = useState(false);
  const [selectedPaths, setSelectedPaths] = useState<Set<string>>(new Set()); // stores keys like `${moduleId}::/path`

  // fetch runtime for selected module
  useEffect(() => {
    let cancelled = false;
    if (!selectedModule) {
      // eslint-disable-next-line react-hooks/set-state-in-effect
      setModuleData(null);
      return;
    }
    setLoading(true);
    getModuleData(selectedModule, 'runtime')
      .then(data => { if (!cancelled) setModuleData(data); })
      .catch(() => { if (!cancelled) setModuleData(null); })
      .finally(() => { if (!cancelled) setLoading(false); });
    return () => { cancelled = true; };
  }, [selectedModule]);

  function togglePath(modId: string, path: string) {
    const key = `${modId}::${path}`;
    setSelectedPaths(prev => { const n = new Set(prev); if (n.has(key)) n.delete(key); else n.add(key); return n; });
  }

  // Note: picker will render the existing DataTree in pickerMode below.

  const confirm = () => {
    const out: WatchEntry[] = [];
    for (const key of selectedPaths) {
      const [modId, p] = key.split('::');
      out.push({ id: makeId(), moduleId: modId, path: p, label: `${modId} ${p}` });
    }
    if (out.length) onConfirm(out);
    else onCancel();
  };

  return (
    <div className="picker-overlay" onClick={e => e.target === e.currentTarget && onCancel()}>
      <div className="picker-modal picker-modal-wide">
        <div className="picker-header"><span>Add Watch</span><button onClick={onCancel}>×</button></div>
        <div className="picker-body picker-body-split">
          <div className="picker-modules">
            {moduleIds.length === 0 && <div className="picker-empty">No modules available</div>}
            {moduleIds.map(mid => {
              return (
                <div key={mid} className={`picker-module-row ${mid === selectedModule ? 'active' : ''}`} onClick={() => setSelectedModule(mid)}>
                  <div className="picker-module-id">{mid}</div>
                </div>
              );
            })}
          </div>
          <div className="picker-tree-area">
            <div className="picker-tree-header">{selectedModule ?? 'Select a module'}</div>
            <div className="picker-tree-body">
              {loading && <div>Loading…</div>}
              {!loading && selectedModule && moduleData && (
                <div>
                  <div className="picker-tree-root">
                    <DataTree
                      data={{ "/": moduleData }}
                      pickerMode={true}
                      selectedKeys={new Set(Array.from(selectedPaths).filter(k => k.startsWith(`${selectedModule}::`)).map(k => k.slice(selectedModule.length + 2)))}
                      onSelect={(pathArr) => {
                        const path = pathArr.join('/').replace(/\/+/g, '/'); // normalize multiple slashes
                        togglePath(selectedModule, path);
                      }}
                    />
                  </div>
                </div>
              )}
            </div>
          </div>
        </div>
        <div className="picker-footer">
          <button onClick={onCancel}>Cancel</button>
          <button disabled={selectedPaths.size === 0} onClick={confirm}>Add {selectedPaths.size > 0 ? `(${selectedPaths.size})` : ''}</button>
        </div>
      </div>
    </div>
  );
}
