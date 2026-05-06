# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.10] - 2026-04-14

### Added
- Modular C++23 runtime with dynamic plugin loading (dlopen/LoadLibrary)
- Header-only SDK (`loom::Module<Config, Recipe, Runtime>`) with automatic struct reflection via glaze
- Real-time scheduler with configurable classes (period, CPU affinity, priority)
- Typed pub/sub bus and synchronous/async RPC services
- Data persistence for config and recipe sections
- HTTP REST API (Crow) for module management, scheduling, oscilloscope, IO mappings, and bus interaction
- WebSocket live streaming (`/ws` and `/ws/watch`)
- React 19 + TypeScript frontend debug IDE with dashboard, oscilloscope, scheduler view, bus explorer, and IO mapping editor
- Built-in example modules: ExampleMotor, PneumaticActuator, StackLight, Sequencer, EtherCAT
- Module template with documentation for creating new modules
- GTest unit and integration tests
- GitHub Actions CI/CD (Ubuntu, macOS, Windows)
- `just` task runner for build, test, run, and packaging workflows

### Security
- Default bind address changed to `127.0.0.1` (localhost only)
- File upload endpoint validates filenames and whitelists module extensions
