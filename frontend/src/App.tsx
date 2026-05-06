import { BrowserRouter, Routes, Route, NavLink } from 'react-router-dom';
import Dashboard from './pages/Dashboard';
import ModuleDetail from './pages/ModuleDetail';
import BusView from './pages/BusView';
import SchedulerView from './pages/SchedulerView';
import OscilloscopeView from './pages/OscilloscopeView';
import WatchView from './pages/WatchView';
import { MappingView } from './pages/MappingView';
import { DataServiceContext, useDataServiceProvider } from './api/dataService';
import './App.css';

function IconModules() {
  return (
    <svg width="15" height="15" viewBox="0 0 15 15" fill="currentColor">
      <rect x="1" y="1" width="5.5" height="5.5" rx="1.2"/>
      <rect x="8.5" y="1" width="5.5" height="5.5" rx="1.2"/>
      <rect x="1" y="8.5" width="5.5" height="5.5" rx="1.2"/>
      <rect x="8.5" y="8.5" width="5.5" height="5.5" rx="1.2"/>
    </svg>
  );
}
function IconScheduler() {
  return (
    <svg width="15" height="15" viewBox="0 0 15 15" fill="currentColor">
      <rect x="1" y="4" width="13" height="10" rx="1.2" fillOpacity="0.25"/>
      <rect x="1" y="1" width="13" height="4" rx="1.2"/>
      <rect x="1" y="7" width="5" height="2" rx="0.5" fillOpacity="0.8"/>
      <rect x="1" y="10.5" width="8" height="2" rx="0.5" fillOpacity="0.6"/>
    </svg>
  );
}
function IconBus() {
  return (
    <svg width="15" height="15" viewBox="0 0 15 15" fill="currentColor">
      <circle cx="3" cy="7.5" r="2"/>
      <circle cx="12" cy="3.5" r="2"/>
      <circle cx="12" cy="11.5" r="2"/>
      <line x1="5" y1="7.5" x2="10" y2="4" stroke="currentColor" strokeWidth="1.2"/>
      <line x1="5" y1="7.5" x2="10" y2="11" stroke="currentColor" strokeWidth="1.2"/>
    </svg>
  );
}
function IconScope() {
  return (
    <svg width="15" height="15" viewBox="0 0 15 15" fill="none" stroke="currentColor" strokeWidth="1.4">
      <polyline points="1,10 4,6 6,8 9,3 12,7 14,5"/>
    </svg>
  );
}

function IconMapping() {
  return (
    <svg width="15" height="15" viewBox="0 0 15 15" fill="none" stroke="currentColor" strokeWidth="1.2">
      <circle cx="3" cy="3" r="1.5" fill="currentColor"/>
      <circle cx="12" cy="12" r="1.5" fill="currentColor"/>
      <path d="M 5 3 L 10 12" strokeLinecap="round"/>
      <circle cx="6" cy="7" r="1" fill="currentColor"/>
    </svg>
  );
}

function AppProviders() {
  const dataService = useDataServiceProvider();
  return (
    <DataServiceContext.Provider value={dataService}>
      <div className="app">
        <header className="app-header">
          <div className="app-logo">
            <div className="app-logo-icon">⚙</div>
            <h1>CrunTime</h1>
          </div>
          <nav>
            <NavLink to="/"><IconModules /><span>Modules</span></NavLink>
            <NavLink to="/scheduler"><IconScheduler /><span>Scheduler</span></NavLink>
            <NavLink to="/bus"><IconBus /><span>Bus</span></NavLink>
            <NavLink to="/scope"><IconScope /><span>Scope</span></NavLink>
            <NavLink to="/watch"><IconScope /><span>Watch</span></NavLink>
            <NavLink to="/mappings"><IconMapping /><span>Mappings</span></NavLink>
          </nav>
          <span
            className={`ws-indicator ${dataService.wsConnected ? 'connected' : 'disconnected'}`}
            title={dataService.wsConnected ? 'WebSocket connected' : 'WebSocket disconnected'}
          />
        </header>
        <main className="app-main">
          <Routes>
            <Route path="/" element={<Dashboard />} />
            <Route path="/module/:id" element={<ModuleDetail />} />
            <Route path="/scheduler" element={<SchedulerView />} />
            <Route path="/bus" element={<BusView />} />
            <Route path="/scope" element={<OscilloscopeView />} />
            <Route path="/watch" element={<WatchView />} />
            <Route path="/mappings" element={<MappingView />} />
          </Routes>
        </main>
      </div>
    </DataServiceContext.Provider>
  );
}

function App() {
  return (
    <BrowserRouter>
      <AppProviders />
    </BrowserRouter>
  );
}

export default App;
