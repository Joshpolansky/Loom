# Loom build tasks

exe_suffix := if os_family() == "windows" { ".exe" } else { "" }

default: build

# First-time setup: install Conan dependencies
setup:
    conan install . --output-folder=build/Debug --build=missing --profile=conan/profiles/debug

setup-release:
    conan install . --output-folder=build/Release --build=missing --profile=conan/profiles/release

# Configure with CMake preset (Conan-generated).
# Run from a Developer Command Prompt (Windows) or any terminal (Linux/macOS).
configure: setup
    cmake --preset conan-debug

# Build (Debug)
build: configure
    cmake --build --preset conan-debug

# Build (Release)
build-release: setup-release
    cmake --preset conan-release
    cmake --build --preset conan-release

# Run tests
test: build
    ctest --preset conan-debug --output-on-failure

# Run the runtime (loads modules from build/Debug/modules/)
run: build
    ./build/Debug/runtime/loom{{exe_suffix}} --module-dir ./build/Debug/modules

# Build the React frontend
frontend:
    cd frontend && npm install && npm run build

# Start the frontend dev server (proxies API to localhost:8080)
dev:
    cd frontend && npm run dev

# Clean build artifacts
clean:
    rm -rf build/ CMakeUserPresets.json

# Package: build and install both Conan packages (SDK + runtime) into the local cache.
# Version is read automatically from the VERSION file — no need to keep it in sync.
package:
    conan create sdk/ --version=$(cat VERSION) --user=local --channel=stable --profile=conan/profiles/debug
    conan create runtime/ --version=$(cat VERSION) --user=local --channel=stable --profile=conan/profiles/debug
