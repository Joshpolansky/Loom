import { useEffect, useRef, useState, useCallback } from 'react';
import type { DragEvent } from 'react';
import { getModuleData } from '../api/rest';
import { useDataService } from '../api/dataService';
import { useVariable, useMachine } from '@loupeteam/lux-react';
import { node } from '../api/machine';
import { DataTree } from '../components/DataTree';
import './WatchView.css';

type WatchEntry = { id: string; moduleId: string; path: string; label: string; expand?: boolean };

const LS_KEY = 'loom:watch:list';

function makeId() { return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`; }

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

// ---------- one live-subscribed watch row ----------
// Each row owns a single OPC-UA monitored item on the runtime field/subtree it
// watches (per-leaf subscription). One hook per row keeps the Rules of Hooks
// happy even though the watch list is dynamic.
function WatchRow({
  entry, onRemove, onDragStart, onDragOver, onDrop, onDragEnd,
}: {
  entry: WatchEntry;
  onRemove: (id: string) => void;
  onDragStart: (e: DragEvent, id: string) => void;
  onDragOver: (e: DragEvent) => void;
  onDrop: (e: DragEvent, id: string) => void;
  onDragEnd: () => void;
}) {
  const [value] = useVariable<unknown>(node(entry.moduleId, 'runtime', entry.path));
  const { writeVariable } = useMachine();

  const pathLabel = entry.path ? `${entry.moduleId}/${entry.path}` : entry.moduleId;
  const data: Record<string, unknown> = { [pathLabel]: value ?? null };

  function handleWrite(localPath: string[], v: unknown) {
    // localPath[0] is the display label; the rest is the pointer within the
    // watched subtree. Absolute runtime pointer = entry.path + that remainder.
    const ptr = [entry.path, ...localPath.slice(1)].filter(Boolean).join('/');
    void writeVariable(node(entry.moduleId, 'runtime', ptr), v);
  }

  return (
    <div
      className="watch-row"
      draggable
      onDragStart={(e) => onDragStart(e, entry.id)}
      onDragOver={onDragOver}
      onDrop={(e) => onDrop(e, entry.id)}
      onDragEnd={onDragEnd}
    >
      <div className="watch-root-header">
        <div className="watch-meta">
          <span className="drag-handle">≡</span>
        </div>
        <DataTree data={data} onChange={handleWrite} />
        <div className="watch-actions">
          <button className="watch-remove-button" onClick={() => onRemove(entry.id)}>✕</button>
        </div>
      </div>
    </div>
  );
}

export default function WatchView() {
  const [showPicker, setShowPicker] = useState(false);
  const [watchList, setWatchList] = useState<WatchEntry[]>(() => loadSaved());
  const dragIdRef = useRef<string | null>(null);
  const { modules } = useDataService();

  // Persist the list; subscriptions are managed per-row by useVariable.
  useEffect(() => { saveList(watchList); }, [watchList]);

  const addEntries = useCallback((selected: WatchEntry[]) => {
    setWatchList((prev) => {
      const next = [...prev];
      for (const s of selected) {
        next.push({ id: s.id ?? makeId(), moduleId: s.moduleId, path: s.path, label: s.label ?? `${s.moduleId} ${s.path}`, expand: s.expand ?? false });
      }
      return next;
    });
    setShowPicker(false);
  }, []);

  const removeEntry = useCallback((id: string) => {
    setWatchList((prev) => prev.filter((p) => p.id !== id));
  }, []);

  const reorder = useCallback((fromId: string, toId: string | null) => {
    setWatchList((prev) => {
      const next = [...prev];
      const fromIdx = next.findIndex((it) => it.id === fromId);
      if (fromIdx === -1) return prev;
      const [item] = next.splice(fromIdx, 1);
      if (!toId) { next.push(item); }
      else {
        const toIdx = next.findIndex((it) => it.id === toId);
        if (toIdx === -1) next.push(item); else next.splice(toIdx, 0, item);
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
        {watchList.map((w) => (
          <WatchRow
            key={w.id}
            entry={w}
            onRemove={removeEntry}
            onDragStart={onDragStart}
            onDragOver={onDragOver}
            onDrop={onDrop}
            onDragEnd={() => { dragIdRef.current = null; }}
          />
        ))}
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
            {moduleIds.map(mid => (
              <div key={mid} className={`picker-module-row ${mid === selectedModule ? 'active' : ''}`} onClick={() => setSelectedModule(mid)}>
                <div className="picker-module-id">{mid}</div>
              </div>
            ))}
          </div>
          <div className="picker-tree-area">
            <div className="picker-tree-header">{selectedModule ?? 'Select a module'}</div>
            <div className="picker-tree-body">
              {loading && <div>Loading…</div>}
              {!loading && selectedModule && moduleData && (
                <div className="picker-tree-root">
                  <DataTree
                    data={{ '/': moduleData }}
                    pickerMode={true}
                    selectedKeys={new Set(Array.from(selectedPaths).filter(k => k.startsWith(`${selectedModule}::`)).map(k => k.slice(selectedModule.length + 2)))}
                    onSelect={(pathArr) => {
                      const path = pathArr.join('/').replace(/\/+/g, '/');
                      togglePath(selectedModule, path);
                    }}
                  />
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
