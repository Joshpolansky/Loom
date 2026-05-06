#pragma once

#include "device.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ── Config — persists across restarts ─────────────────────────────────────────

struct EtherCatConfig {
    // Network interface for EtherCAT frames.
    // macOS: "en4", "en5", etc. (run `networksetup -listallhardwareports`)
    // Linux: "eth0", "enp3s0", etc.
    std::string interface = "en4";

    // Attempt automatic device recovery when the working counter drops.
    bool recover_on_lost_link = true;

    // Initialise and start the main controller automatically on module load.
    bool auto_start = true;

    // Optional per-device PDO overrides applied at init time (PreOp state).
    // Leave empty to use each device's default PDO mapping.
    // Example JSON config:
    // "device_configs": [{ "device_index": 1,
    //   "tx_pdos": [{ "pdo_index": 6656,
    //                 "entries":  [{"index":24641,"subindex":0,"bit_length":16},
    //                              {"index":24676,"subindex":0,"bit_length":32}]}]}]
    std::vector<ethermouse::DevicePdoConfig> device_configs = {};
};

// ── Recipe — swap at runtime without restarting ───────────────────────────────

struct EtherCatRecipe {
    // Placeholder: extend with product-specific device setpoints, e.g.:
    //   std::vector<double> target_positions = {};
};

// ── Runtime — the live process image, updated every preCyclic/postCyclic ──────

struct DeviceStatus {
    // ── Identity ──────────────────────────────────────────────────────────
    int         index         = 0;
    std::string name          = {};
    uint32_t    vendor_id     = 0;
    uint32_t    product_code  = 0;
    uint32_t    revision      = 0;
    bool        in_op         = false;

    // ── PDO sizes and byte offsets within the main controller's flat PDO buffers ───
    // Use input_offset/output_offset to index into EtherCatRuntime::inputs/outputs
    // for direct RT-safe access. The per-device inputs/outputs below are a 10 Hz copy.
    uint16_t    input_bytes   = 0;
    uint16_t    output_bytes  = 0;
    uint32_t    input_offset  = 0;
    uint32_t    output_offset = 0;

    // ── Per-device process image (10 Hz copy from main controller buffers) ──
    std::vector<uint8_t> inputs  = {};   // current PDO data from this device
    std::vector<uint8_t> outputs = {};   // current PDO data to this device

    // ── Signal map (read once via CoE SDO when device list changes) ────────
    std::vector<ethermouse::PdoSignal> input_signals  = {};  // TxPDO signals
    std::vector<ethermouse::PdoSignal> output_signals = {};  // RxPDO signals
};

struct EtherCatRuntime {

    bool enabled = false;  // set true to allow cyclic() to exchange PDOs with the network

    // ── Process image ──────────────────────────────────────────────────────
    // Raw PDO buffers — the actual data exchanged with the fieldbus each cycle.
    //
    // inputs:  populated by preCyclic() from the network receive buffer.
    //          Read these in cyclic() to get current sensor/encoder values.
    //
    // outputs: written in cyclic() to drive actuators.
    //          Flushed to the network by postCyclic().
    //
    // For a specific machine, layer typed device structs over these bytes:
    //   auto& motor = *reinterpret_cast<MyMotorPdo*>(runtime_.inputs.data());
    std::vector<uint8_t> inputs  = {};
    std::vector<uint8_t> outputs = {};

    // ── Fieldbus health ────────────────────────────────────────────────────
    int  working_counter = 0;   // actual WKC from last receive
    int  expected_wkc    = 0;   // expected WKC (all devices responding)
    bool wkc_ok          = false;

    // ── Controller diagnostics ─────────────────────────────────────────────
    // "idle" | "starting" | "running" | "degraded" | "error"
    std::string status     = "idle";
    std::string last_error = "";

    bool     running       = false;
    bool     all_op        = false;
    int      device_count  = 0;
    uint64_t cycle_count   = 0;

    std::vector<DeviceStatus> devices = {};
};
