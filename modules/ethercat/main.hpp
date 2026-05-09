#pragma once

#include "device.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ethermouse {

// Callback invoked after each cyclic PDO exchange.
// outputs: process data written TO devices (read-only view for logging)
// inputs:  process data read FROM devices
using CyclicCallback = std::function<void(
    std::span<const uint8_t> outputs,
    std::span<const uint8_t> inputs
)>;

struct MainConfig {
    std::string interface;          // macOS: "en4"/"en5" etc.  Linux: "eth0"/"enp3s0" etc.
    std::chrono::microseconds cycle_time{1000};  // default 1 kHz
    int  rt_priority{80};           // SCHED_FIFO priority (0 = no RT)
    bool recover_on_lost_link{true};

    // Optional per-device PDO overrides applied during init() (PreOp state).
    // Leave empty to use each device's default PDO mapping.
    std::vector<DevicePdoConfig> device_pdos;
};

class Main {
public:
    explicit Main(MainConfig config);
    ~Main();

    // Non-copyable, movable
    Main(const Main&) = delete;
    Main& operator=(const Main&) = delete;
    Main(Main&&) noexcept;
    Main& operator=(Main&&) noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    // Initialise the NIC and scan for devices. Returns device count.
    // Throws std::runtime_error on failure.
    int init();

    // Bring all devices to OP and start the internal cyclic thread.
    // The thread calls tick_send()/tick_receive() at config.cycle_time intervals.
    void start(CyclicCallback cb = nullptr);

    // Bring all devices to OP without starting the internal cyclic thread.
    // The caller is responsible for driving tick_send()/tick_receive() each
    // cycle (e.g. from Loom preCyclic/postCyclic hooks).
    void start_manual();

    // Request a clean stop: bring devices to Init, join thread (if running).
    void stop();

    bool is_running() const noexcept;

    // ── Manual tick mode ──────────────────────────────────────────────────
    // Receive the PDO input frame. Returns the working counter.
    // wkc < expected_wkc() means at least one device is missing or in error.
    // Call from preCyclic() before reading input_pdo().
    int tick_receive() noexcept;

    // Send the PDO output frame to all devices.
    // Call from postCyclic() after writing output_pdo().
    void tick_send() noexcept;

    // Expected working counter: outputsWKC*2 + inputsWKC for group 0.
    int expected_wkc() const noexcept;

    // ── Device access ─────────────────────────────────────────────────────
    int device_count() const noexcept;

    // Returns info snapshot for device at 1-based index.
    DeviceInfo device_info(int index) const;

    const std::vector<DeviceInfo>& devices() const noexcept;

    // ── Process data (call only from cyclic callback or after stop()) ─────
    // Span over the entire output PDO buffer (host → devices).
    std::span<uint8_t> output_pdo() noexcept;

    // Span over the entire input PDO buffer (devices → host).
    std::span<const uint8_t> input_pdo() const noexcept;

    // ── Non-cyclic SDO access (CoE mailbox — not RT safe) ────────────────────
    // Read any object dictionary entry. Returns the raw bytes, or empty on error.
    // max_size: maximum expected payload (bytes). 4 covers most scalars; use 256
    //           for strings or arrays.
    std::vector<uint8_t> sdo_read(int device_index, uint16_t index,
                                   uint8_t subindex, int max_size = 4);

    // Write any object dictionary entry. data/size are the raw payload bytes.
    // Returns true on success.
    bool sdo_write(int device_index, uint16_t index, uint8_t subindex,
                   const void* data, int size);

    // Typed convenience wrappers.
    template<typename T>
    std::optional<T> sdo_read_as(int device_index, uint16_t index, uint8_t subindex) {
        auto bytes = sdo_read(device_index, index, subindex, static_cast<int>(sizeof(T)));
        if (bytes.size() < sizeof(T)) return std::nullopt;
        T value{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }

    template<typename T>
    bool sdo_write_value(int device_index, uint16_t index, uint8_t subindex, T value) {
        return sdo_write(device_index, index, subindex, &value, static_cast<int>(sizeof(T)));
    }

    // ── PDO signal introspection (CoE SDO reads — not RT safe) ───────────────
    // Returns signals mapped into the device's input PDO (TxPDO, device→main).
    // Reads 0x1C13 + 0x1A00-0x1BFF from the device's object dictionary.
    // Call from longRunning() or at init time; never from cyclic/pre/postCyclic.
    std::vector<PdoSignal> read_input_signals(int device_index);

    // Returns signals mapped into the device's output PDO (RxPDO, main→device).
    // Reads 0x1C12 + 0x1600-0x17FF from the device's object dictionary.
    std::vector<PdoSignal> read_output_signals(int device_index);

    // ── Diagnostics ───────────────────────────────────────────────────────
    uint64_t cycle_count()    const noexcept;
    uint64_t missed_cycles()  const noexcept;
    bool     all_op()         const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ethermouse
