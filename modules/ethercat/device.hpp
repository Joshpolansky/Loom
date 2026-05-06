#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ethermouse {

enum class DeviceState : uint8_t {
    None     = 0x00,
    Init     = 0x01,
    PreOp    = 0x02,
    Boot     = 0x03,
    SafeOp   = 0x04,
    Op       = 0x08,
    Error    = 0x10,  // OR'd with above when device is in error
};

// ── PDO configuration (applied at init time, before ecx_config_map_group) ────
//
// Use these to override the device's default PDO mapping.  Fill in
// EtherCatConfig::device_configs and they will be written via SDO during
// Main::init() while the device is in PreOp state.
//
// Example — map only StatusWord + ActualPosition on device 1 TxPDO:
//   DevicePdoConfig cfg;
//   cfg.device_index = 1;
//   cfg.tx_pdos = {{ .pdo_index = 0x1A00,
//                    .entries   = {{ 0x6041, 0x00, 16 },   // StatusWord
//                                  { 0x6064, 0x00, 32 }}   // ActualPosition
//                 }};

struct PdoConfigEntry {
    uint16_t index      = 0;  // CoE object index
    uint8_t  subindex   = 0;
    uint16_t bit_length = 0;
};

struct PdoConfigMapping {
    uint16_t                   pdo_index = 0;     // e.g. 0x1A00 (TxPDO) or 0x1600 (RxPDO)
    std::vector<PdoConfigEntry> entries;
};

struct DevicePdoConfig {
    int                          device_index = 1;  // 1-based
    std::vector<PdoConfigMapping> tx_pdos;           // inputs  (device → main, 0x1C13 / 0x1A00+)
    std::vector<PdoConfigMapping> rx_pdos;           // outputs (main → device, 0x1C12 / 0x1600+)
};

// ── PDO signal descriptor (read-only, returned by read_*_signals) ─────────────
// One signal (PDO entry) within a device's process image.
// Obtained via read_input_signals() / read_output_signals() — CoE SDO reads,
// not RT safe. Cache the result; only re-read when the device list changes.
struct PdoSignal {
    std::string name;           // "0x6041:00" or richer name if OD provides one
    uint16_t    index      = 0; // CoE object index
    uint8_t     subindex   = 0; // CoE sub-index
    uint16_t    bit_length = 0; // signal width in bits
    uint32_t    bit_offset = 0; // bit offset within this device's PDO region
};

struct DeviceInfo {
    int         index           = 0; // 1-based SOEM device index
    std::string name;
    uint32_t    vendor_id       = 0;
    uint32_t    product_code    = 0;
    uint32_t    revision_number = 0;
    DeviceState state           = DeviceState::None;
    uint16_t    output_bytes    = 0;  // process data output size (main → device)
    uint16_t    input_bytes     = 0;  // process data input  size (device → main)

    // Byte offsets within the main controller's flat PDO buffers.
    // Use these to slice runtime_.inputs / runtime_.outputs per device.
    uint32_t    input_offset    = 0;
    uint32_t    output_offset   = 0;

    bool has_error() const noexcept {
        return static_cast<uint8_t>(state) & static_cast<uint8_t>(DeviceState::Error);
    }
};

}  // namespace ethermouse
