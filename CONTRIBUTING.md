# Contributing to Loom

Thanks for your interest in contributing to Loom! This guide will help you get started.

## Prerequisites

- CMake >= 3.25
- C++23 compiler (clang++ 17+ or g++ 13+)
- [Conan](https://conan.io/) 2.x
- [just](https://github.com/casey/just) task runner
- Node.js 18+

## Getting Started

```bash
# Clone the repo
git clone https://github.com/Joshpolansky/loom.git
cd Loom

# Build the frontend
just frontend

# Build the C++ runtime + modules
just build

# Run the runtime
just run
```

For frontend development with hot reload:

```bash
just run         # Terminal 1 — runtime on :8080
just dev         # Terminal 2 — Vite dev server on :5173
```

## Running Tests

```bash
just test
```

This builds the project (if needed) and runs the GTest suite.

## Project Structure

| Directory | Description |
|-----------|-------------|
| `sdk/` | Header-only C++23 SDK for module authors |
| `runtime/` | Core runtime engine + HTTP/WebSocket server |
| `modules/` | Built-in example modules |
| `module-template/` | Starter template for writing new modules |
| `frontend/` | React + TypeScript debug IDE |
| `tests/` | GTest unit and integration tests |
| `data/` | Instance configs, scheduler settings, UI assets |

## Code Style

- **C++**: Modern C++23 — use aggregates, `std::optional`, structured bindings. No macros for data registration (glaze handles reflection automatically).
- **TypeScript**: Standard React + TypeScript conventions. Use functional components and hooks.
- **Commits**: Use [conventional commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`).

## Writing a Module

The best way to learn is from the `module-template/` directory. See its [README](module-template/README.md) for a step-by-step guide.

Key rules:
- Data structs (`Config`, `Recipe`, `Runtime`) must be plain C++23 aggregates (no custom constructors).
- Use `LOOM_MODULE_HEADER` and `LOOM_REGISTER_MODULE` macros for module registration.
- Use the bus API (`publishLocal`, `subscribeTo`, `registerLocalService`) for inter-module communication.

## Pull Request Process

1. Fork the repository and create a branch from `main`.
2. Make your changes, following the code style above.
3. Add or update tests if applicable.
4. Ensure `just test` passes.
5. Open a pull request against `main` with a clear description of the change.

## Reporting Issues

Use [GitHub Issues](https://github.com/Joshpolansky/loom/issues) to report bugs or request features. Please include:
- Steps to reproduce (for bugs)
- Expected vs. actual behavior
- Platform and compiler version

## License

By contributing, you agree that your contributions will be licensed under the [Apache License 2.0](LICENSE).
