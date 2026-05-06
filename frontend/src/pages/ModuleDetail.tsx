import { useEffect, useState } from 'react';
import { useParams, Link, useNavigate } from 'react-router-dom';
import { reloadModule, reassignModuleClass, getSchedulerClasses, saveModuleConfig, loadModuleConfig, saveModuleRecipe, loadModuleRecipe, removeModuleInstance } from '../api/rest';
import { useDataService } from '../api/dataService';
import { SectionPanel } from '../components/SectionPanel';
import { ModuleRpcPanel } from '../components/ModuleRpcPanel';
import { MODULE_STATES } from '../types';
import type { DataSection, ClassInfo } from '../types';
import './ModulesDetails.css';
const SECTIONS: { key: DataSection; label: string; readOnly?: boolean }[] = [
  { key: 'summary', label: 'Summary', readOnly: true },
  { key: 'runtime', label: 'Runtime' },
  { key: 'config',  label: 'Config' },
  { key: 'recipe',  label: 'Recipe' },
];

type ActiveTab = DataSection;

export default function ModuleDetail() {
  const { id } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const { modules, wsConnected, fetchDetail, writeField, subscribeRuntime, unsubscribeRuntime } = useDataService();
  const [activeTab, setActiveTab] = useState<ActiveTab>('runtime');
  const activeSection: DataSection = activeTab;
  const [reloading, setReloading] = useState(false);
  const [removing, setRemoving] = useState(false);
  const [error, setError] = useState('');
  const [classes, setClasses] = useState<ClassInfo[]>([]);
  const [reassigning, setReassigning] = useState(false);
  const [recipeName, setRecipeName] = useState('default');
  const [diskBusy, setDiskBusy] = useState(false);
  const [diskMsg, setDiskMsg] = useState('');

  const mod = id ? modules[id] : undefined;

  // Fetch full detail on mount / when id changes
  useEffect(() => {
    if (id) fetchDetail(id);
  }, [id, fetchDetail]);

  // Subscribe to this module's live runtime stream while the page is open.
  // The server only ships per-module runtime data over /ws to subscribers,
  // so without this we'd only ever see the initial REST snapshot.
  useEffect(() => {
    if (!id) return;
    subscribeRuntime(id);
    return () => { unsubscribeRuntime(id); };
  }, [id, subscribeRuntime, unsubscribeRuntime]);

  // Fetch available classes for the selector
  useEffect(() => {
    getSchedulerClasses().then(setClasses).catch(() => {});
  }, []);

  async function handleReload() {
    if (!id) return;
    setReloading(true);
    setError('');
    try {
      await reloadModule(id);
      await fetchDetail(id);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setReloading(false);
    }
  }

  async function handleRemove() {
    if (!id) return;
    if (!confirm(`Remove instance "${id}"? This cannot be undone.`)) return;
    setRemoving(true);
    setError('');
    try {
      await removeModuleInstance(id);
      navigate('/');
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
      setRemoving(false);
    }
  }

  async function handleReassign(newClass: string) {
    if (!id) return;
    setReassigning(true);
    setError('');
    try {
      await reassignModuleClass(id, newClass);
      await fetchDetail(id);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setReassigning(false);
    }
  }

  async function handleSaveConfig() {
    if (!id) return;
    setDiskBusy(true); setDiskMsg('');
    try { await saveModuleConfig(id); setDiskMsg('Config saved.'); }
    catch (e: unknown) { setDiskMsg(e instanceof Error ? e.message : String(e)); }
    finally { setDiskBusy(false); }
  }

  async function handleLoadConfig() {
    if (!id) return;
    setDiskBusy(true); setDiskMsg('');
    try { await loadModuleConfig(id); await fetchDetail(id); setDiskMsg('Config loaded.'); }
    catch (e: unknown) { setDiskMsg(e instanceof Error ? e.message : String(e)); }
    finally { setDiskBusy(false); }
  }

  async function handleSaveRecipe() {
    if (!id) return;
    setDiskBusy(true); setDiskMsg('');
    try { await saveModuleRecipe(id, recipeName); setDiskMsg(`Recipe "${recipeName}" saved.`); }
    catch (e: unknown) { setDiskMsg(e instanceof Error ? e.message : String(e)); }
    finally { setDiskBusy(false); }
  }

  async function handleLoadRecipe() {
    if (!id) return;
    setDiskBusy(true); setDiskMsg('');
    try { await loadModuleRecipe(id, recipeName); await fetchDetail(id); setDiskMsg(`Recipe "${recipeName}" loaded.`); }
    catch (e: unknown) { setDiskMsg(e instanceof Error ? e.message : String(e)); }
    finally { setDiskBusy(false); }
  }

  if (!mod) return <div className="loading">Loading…</div>;

  const { info, detail, liveRuntime, liveSummary } = mod;

  const sectionData = (section: DataSection): Record<string, unknown> => {
    if (section === 'runtime') return liveRuntime;
    if (section === 'summary') return liveSummary;
    return detail?.data[section] ?? {};
  };

  return (
    <div className="detail-layout">
      <Link to="/" className="back-link">← Back to modules</Link>

      <div className="detail-header">
        <h2>
          {info.id}{info.className && info.className !== info.id && <span className="detail-type"> ({info.className})</span>}{' '}
          <span className={`state-badge state-${info.state}`}>
            {MODULE_STATES[info.state] ?? 'Unknown'}
          </span>
          <span className={`ws-indicator ${wsConnected ? 'connected' : 'disconnected'}`} />
        </h2>
        <div className="detail-actions">
          {classes.length > 0 && (
            <div className="class-selector">
              <label>Class</label>
              <select
                value={info.cyclicClass ?? ''}
                disabled={reassigning}
                onChange={(e) => handleReassign(e.target.value)}
              >
                {info.cyclicClass == null && <option value="">—</option>}
                {classes.map((c) => (
                  <option key={c.name} value={c.name}>{c.name} ({c.period_us >= 1000 ? `${c.period_us / 1000}ms` : `${c.period_us}µs`})</option>
                ))}
              </select>
              {reassigning && <span className="reassign-spinner">…</span>}
            </div>
          )}
          <button className="btn danger" onClick={handleReload} disabled={reloading}>
            {reloading ? 'Reloading…' : 'Reload'}
          </button>
          <button className="btn danger" onClick={handleRemove} disabled={removing || reloading}
            title="Remove this instance permanently">
            {removing ? 'Removing…' : 'Remove'}
          </button>
        </div>
      </div>

      <div className="meta" style={{ marginBottom: '1rem' }}>
        <span>Type: {info.className || info.name}</span> &middot;{' '}
        <span>v{info.version}</span> &middot;{' '}
        <span className='module-path'>{info.path}</span>
      </div>

      {error && <div className="error-msg">{error}</div>}

      <div className="tabs">
        {SECTIONS.map(({ key, label }) => (
          <button
            key={key}
            className={`tab ${activeTab === key ? 'active' : ''}`}
            onClick={() => setActiveTab(key)}
          >
            {label}
          </button>
        ))}
      </div>

      {(activeSection === 'config' || activeSection === 'recipe') && (
        <div className="disk-toolbar">
          {activeSection === 'recipe' && (
            <input
              className="recipe-name-input"
              value={recipeName}
              onChange={(e) => setRecipeName(e.target.value)}
              placeholder="recipe name"
              disabled={diskBusy}
            />
          )}
          <button className="btn secondary" onClick={activeSection === 'config' ? handleLoadConfig : handleLoadRecipe} disabled={diskBusy}>
            Load from disk
          </button>
          <button className="btn secondary" onClick={activeSection === 'config' ? handleSaveConfig : handleSaveRecipe} disabled={diskBusy}>
            Save to disk
          </button>
          {diskMsg && <span className="disk-msg">{diskMsg}</span>}
        </div>
      )}

      <div className={activeTab === 'runtime' ? 'runtime-split' : undefined}>
        <SectionPanel
          title={SECTIONS.find((s) => s.key === activeSection)!.label}
          data={sectionData(activeSection)}
          section={activeSection}
          onWrite={
            !SECTIONS.find((s) => s.key === activeSection)?.readOnly && id
              ? (path, value) => writeField(id, activeSection, path, value)
              : undefined
          }
        />
        {activeTab === 'runtime' && <ModuleRpcPanel moduleId={id!} />}
      </div>
    </div>
  );
}

