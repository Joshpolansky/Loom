import { useEffect, useState } from 'react';
import { getModuleData } from '../api/rest';
import { useDataService } from '../api/dataService';
import { DataTree } from '../components/DataTree';
import './MappingView.css';

interface IOMapping {
  index?: number;
  source: string;
  target: string;
  enabled: boolean;
  status?: string; // 'resolved', 'error', etc.
  error?: string;
}

export function MappingView() {
  const {
    modules,
  } = useDataService();

  const [mappings, setMappings] = useState<IOMapping[]>([]);
  const [loading, setLoading] = useState(true);
  const [showPicker, setShowPicker] = useState(false);

  // Load mappings on mount
  useEffect(() => {
    loadMappings();
    const interval = setInterval(loadMappings, 2000);
    return () => clearInterval(interval);
  }, []);

  async function loadMappings() {
    try {
      setLoading(true);
      const response = await fetch('/api/io-mappings');
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      const data = await response.json();
      setMappings(Array.isArray(data) ? data : []);
    } catch (err) {
      console.error('Failed to load mappings:', err);
    } finally {
      setLoading(false);
    }
  }

  async function deleteMapping(index: number) {
    try {
      const response = await fetch(`/api/io-mappings/${index}`, { method: 'DELETE' });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      await loadMappings();
    } catch (err) {
      console.error('Failed to delete mapping:', err);
    }
  }

  async function toggleMapping(index: number, enabled: boolean) {
    try {
      const mapping = mappings[index];
      if (!mapping) return;
      const response = await fetch(`/api/io-mappings/${index}`, {
        method: 'PATCH',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          source: mapping.source,
          target: mapping.target,
          enabled: !enabled,
        }),
      });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      await loadMappings();
    } catch (err) {
      console.error('Failed to update mapping:', err);
    }
  }

  async function addMapping(source: string, target: string) {
    try {
      const response = await fetch('/api/io-mappings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          source,
          target,
          enabled: true,
        }),
      });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      await loadMappings();
      setShowPicker(false);
    } catch (err) {
      console.error('Failed to add mapping:', err);
    }
  }

  async function resolveMappings() {
    try {
      const response = await fetch('/api/io-mappings/resolve', { method: 'POST' });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      await loadMappings();
    } catch (err) {
      console.error('Failed to resolve mappings:', err);
    }
  }

  return (
    <div className="mapping-page">
      <div className="mapping-header">
        <h2>I/O Mappings</h2>
        <div className="mapping-controls">
          <button onClick={() => setShowPicker(true)} className="mapping-add-button">
            + Add Mapping
          </button>
          <button onClick={resolveMappings} className="mapping-resolve-button">
            Resolve
          </button>
        </div>
      </div>

      <div className="mapping-list">
        {loading && <div className="mapping-loading">Loading mappings…</div>}
        {!loading && mappings.length === 0 && (
          <div className="mapping-empty">No mappings configured — click Add Mapping.</div>
        )}

        {!loading &&
          mappings.map((mapping, idx) => (
            <div key={idx} className="mapping-row">
              <div className="mapping-content">
                <div className="mapping-fields">
                  <div className="mapping-field mapping-source">
                    <div className="mapping-field-label">Source</div>
                    <code className="mapping-field-value">{mapping.source}</code>
                  </div>
                  <div className="mapping-arrow">→</div>
                  <div className="mapping-field mapping-target">
                    <div className="mapping-field-label">Target</div>
                    <code className="mapping-field-value">{mapping.target}</code>
                  </div>
                </div>
                <div className="mapping-status">
                  <span className={`status-badge status-${mapping.status || 'unknown'}`}>
                    {mapping.status || 'unknown'}
                  </span>
                  {mapping.error && <span className="status-error">{mapping.error}</span>}
                </div>
              </div>
              <div className="mapping-actions">
                <button
                  onClick={() => toggleMapping(idx, mapping.enabled)}
                  className={`mapping-toggle-button ${mapping.enabled ? 'enabled' : 'disabled'}`}
                  title={mapping.enabled ? 'Disable' : 'Enable'}
                >
                  {mapping.enabled ? '●' : '○'}
                </button>
                <button
                  onClick={() => deleteMapping(idx)}
                  className="mapping-delete-button"
                  title="Delete"
                >
                  ✕
                </button>
              </div>
            </div>
          ))}
      </div>

      {showPicker && (
        <MappingPickerModal
          modules={modules}
          onConfirm={(source, target) => {
            addMapping(source, target);
          }}
          onCancel={() => setShowPicker(false)}
        />
      )}
    </div>
  );
}

interface PickerModalProps {
  modules: Record<string, unknown>;
  onConfirm: (source: string, target: string) => void;
  onCancel: () => void;
}

function MappingPickerModal({ modules, onConfirm, onCancel }: PickerModalProps) {
  const moduleIds = Object.keys(modules ?? {}).sort();

  const [sourceModule, setSourceModule] = useState<string | null>(moduleIds.length ? moduleIds[0] : null);
  const [targetModule, setTargetModule] = useState<string | null>(moduleIds.length ? moduleIds[0] : null);
  const [sourceField, setSourceField] = useState<string | null>(null);
  const [targetField, setTargetField] = useState<string | null>(null);
  const [sourceData, setSourceData] = useState<Record<string, unknown> | null>(null);
  const [targetData, setTargetData] = useState<Record<string, unknown> | null>(null);
  const [sourceSection, setSourceSection] = useState<'runtime' | 'config' | 'recipe'>('runtime');
  const [targetSection, setTargetSection] = useState<'runtime' | 'config' | 'recipe'>('runtime');
  const [loadingSource, setLoadingSource] = useState(false);
  const [loadingTarget, setLoadingTarget] = useState(false);

  // Load source module data
  useEffect(() => {
    let cancelled = false;
    if (!sourceModule) {
      // eslint-disable-next-line react-hooks/set-state-in-effect
      setSourceData(null);
      return;
    }
    setLoadingSource(true);
    setSourceField(null);
    getModuleData(sourceModule, sourceSection)
      .then((data: Record<string, unknown>) => {
        if (!cancelled) setSourceData(data);
      })
      .catch(() => {
        if (!cancelled) setSourceData(null);
      })
      .finally(() => {
        if (!cancelled) setLoadingSource(false);
      });
    return () => {
      cancelled = true;
    };
  }, [sourceModule, sourceSection]);

  // Load target module data
  useEffect(() => {
    let cancelled = false;
    if (!targetModule) {
      // eslint-disable-next-line react-hooks/set-state-in-effect
      setTargetData(null);
      return;
    }
    setLoadingTarget(true);
    setTargetField(null);
    getModuleData(targetModule, targetSection)
      .then((data: Record<string, unknown>) => {
        if (!cancelled) setTargetData(data);
      })
      .catch(() => {
        if (!cancelled) setTargetData(null);
      })
      .finally(() => {
        if (!cancelled) setLoadingTarget(false);
      });
    return () => {
      cancelled = true;
    };
  }, [targetModule, targetSection]);

  function handleConfirm() {
    if (sourceModule && sourceField && targetModule && targetField) {
      const source = `${sourceModule}.${sourceSection}.${sourceField}`;
      const target = `${targetModule}.${targetSection}.${targetField}`;
      onConfirm(source, target);
    }
  }

  const canConfirm = sourceModule && sourceField && targetModule && targetField;

  return (
    <div className="mapping-picker-overlay" onClick={(e) => e.target === e.currentTarget && onCancel()}>
      <div className="mapping-picker-modal">
        <div className="mapping-picker-header">
          <span>Add I/O Mapping</span>
          <button onClick={onCancel}>×</button>
        </div>

        <div className="mapping-picker-body">
          {/* Source selector */}
          <div className="mapping-picker-column">
            <div className="mapping-picker-column-title">Source Field</div>
            <div className="mapping-picker-section-selector">
              {['runtime', 'config', 'recipe'].map((sec) => (
                <button
                  key={sec}
                  className={`mapping-section-button ${sourceSection === sec ? 'active' : ''}`}
                  onClick={() => setSourceSection(sec as typeof sourceSection)}
                >
                  {sec}
                </button>
              ))}
            </div>
            <div className="mapping-picker-modules">
              {moduleIds.length === 0 && <div className="mapping-picker-empty">No modules</div>}
              {moduleIds.map((mid) => (
                <div
                  key={mid}
                  className={`mapping-picker-module-row ${mid === sourceModule ? 'active' : ''}`}
                  onClick={() => setSourceModule(mid)}
                >
                  {mid}
                </div>
              ))}
            </div>
            <div className="mapping-picker-tree-area">
              {loadingSource && <div>Loading…</div>}
              {!loadingSource && sourceData && (
                <div className="mapping-picker-tree">
                  <DataTree
                    data={{ root: sourceData }}
                    pickerMode={true}
                    selectedKeys={sourceField ? new Set([sourceField]) : new Set()}
                    onSelect={(pathArr) => {
                      // join path excluding 'root' prefix
                      const path = pathArr.slice(1).join('/');
                      setSourceField(path);
                    }}
                  />
                </div>
              )}
            </div>
          </div>

          {/* Target selector */}
          <div className="mapping-picker-column">
            <div className="mapping-picker-column-title">Target Field</div>
            <div className="mapping-picker-section-selector">
              {['runtime', 'config', 'recipe'].map((sec) => (
                <button
                  key={sec}
                  className={`mapping-section-button ${targetSection === sec ? 'active' : ''}`}
                  onClick={() => setTargetSection(sec as typeof targetSection)}
                >
                  {sec}
                </button>
              ))}
            </div>
            <div className="mapping-picker-modules">
              {moduleIds.length === 0 && <div className="mapping-picker-empty">No modules</div>}
              {moduleIds.map((mid) => (
                <div
                  key={mid}
                  className={`mapping-picker-module-row ${mid === targetModule ? 'active' : ''}`}
                  onClick={() => setTargetModule(mid)}
                >
                  {mid}
                </div>
              ))}
            </div>
            <div className="mapping-picker-tree-area">
              {loadingTarget && <div>Loading…</div>}
              {!loadingTarget && targetData && (
                <div className="mapping-picker-tree">
                  <DataTree
                    data={{ root: targetData }}
                    pickerMode={true}
                    selectedKeys={targetField ? new Set([targetField]) : new Set()}
                    onSelect={(pathArr) => {
                      const path = pathArr.slice(1).join('/');
                      setTargetField(path);
                    }}
                  />
                </div>
              )}
            </div>
          </div>
        </div>

        <div className="mapping-picker-footer">
          <button onClick={onCancel}>Cancel</button>
          <button disabled={!canConfirm} onClick={handleConfirm}>
            Create Mapping
          </button>
        </div>
      </div>
    </div>
  );
}
