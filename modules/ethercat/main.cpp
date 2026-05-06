#include "main.hpp"

// SOEM context-based API
extern "C" {
#include "soem/soem.h"
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef __linux__
#  include <pthread.h>
#  include <sched.h>
#endif

using namespace std::chrono;

namespace ethermouse {

// ── Helpers ───────────────────────────────────────────────────────────────────

static DeviceState soem_state_to_enum(uint16_t s) noexcept {
    return static_cast<DeviceState>(s & 0x1F);
}

static void set_thread_realtime(int priority) noexcept {
#ifdef __linux__
    if (priority <= 0) return;
    sched_param sp{};
    sp.sched_priority = priority;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    // Non-fatal if this fails (e.g. missing CAP_SYS_NICE).
#else
    (void)priority;
#endif
}

// PDO map buffer size — 4 kB covers most applications.
// Increase if you have many devices or large PDOs.
static constexpr std::size_t kPdoMapSize = 4096;

// ── Impl ──────────────────────────────────────────────────────────────────────

struct Main::Impl {
    MainConfig              config;
    std::vector<DeviceInfo> devices;

    ecx_contextt            ctx{};
    uint8_t                 pdo_map[kPdoMapSize]{};  // owned by SOEM after config_map_group

    std::atomic<bool>       running{false};
    std::atomic<uint64_t>   cycle_count{0};
    std::atomic<uint64_t>   missed_cycles{0};

    std::thread             cyclic_thread;
    CyclicCallback          callback;

    // Group 0 aggregates all devices.
    ec_groupt& grp() noexcept { return ctx.grouplist[0]; }

    uint8_t* output_buf()  noexcept { return grp().outputs; }
    uint8_t* input_buf()   noexcept { return grp().inputs; }
    uint32_t output_size() noexcept { return grp().Obytes; }
    uint32_t input_size()  noexcept { return grp().Ibytes; }
    int      expected_wkc() noexcept { return grp().outputsWKC * 2 + grp().inputsWKC; }

    void populate_device_list() {
        devices.clear();
        devices.reserve(static_cast<size_t>(ctx.slavecount));

        const uint8_t* grp_inputs  = grp().inputs;
        const uint8_t* grp_outputs = grp().outputs;

        for (int i = 1; i <= ctx.slavecount; ++i) {
            ec_slavet& s = ctx.slavelist[i];
            DeviceInfo info{};
            info.index           = i;
            info.name            = s.name;
            info.vendor_id       = s.eep_man;
            info.product_code    = s.eep_id;
            info.revision_number = s.eep_rev;
            info.state           = soem_state_to_enum(s.state);
            info.output_bytes    = s.Obytes;
            info.input_bytes     = s.Ibytes;

            // Byte offsets within the main controller's flat PDO group buffers.
            // Valid only after ecx_config_map_group(); zero when no PDO.
            if (s.inputs  && grp_inputs  && s.Ibytes > 0)
                info.input_offset  = static_cast<uint32_t>(s.inputs  - grp_inputs);
            if (s.outputs && grp_outputs && s.Obytes > 0)
                info.output_offset = static_cast<uint32_t>(s.outputs - grp_outputs);

            devices.push_back(std::move(info));
        }
    }

    // Try to recover devices that fell out of OP.
    void check_and_recover_devices() {
        ec_groupt& g = grp();
        g.docheckstate = FALSE;
        ecx_readstate(&ctx);

        for (int i = 1; i <= ctx.slavecount; ++i) {
            ec_slavet& s = ctx.slavelist[i];
            if (s.group != 0) continue;

            if (s.state == EC_STATE_OPERATIONAL) {
                if (s.islost) {
                    s.islost = FALSE;
                }
                continue;
            }

            g.docheckstate = TRUE;

            if (s.state == EC_STATE_SAFE_OP + EC_STATE_ERROR) {
                s.state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                ecx_writestate(&ctx, i);
            } else if (s.state == EC_STATE_SAFE_OP) {
                s.state = EC_STATE_OPERATIONAL;
                ecx_writestate(&ctx, i);
            } else if (s.state > EC_STATE_NONE) {
                if (ecx_reconfig_slave(&ctx, i, EC_TIMEOUTRET)) {
                    s.islost = FALSE;
                }
            } else if (!s.islost) {
                ecx_statecheck(&ctx, i, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                if (s.state == EC_STATE_NONE) {
                    s.islost = TRUE;
                }
            } else if (ecx_recover_slave(&ctx, i, EC_TIMEOUTRET)) {
                s.islost = FALSE;
            }
        }
    }

    void cyclic_loop() {
        set_thread_realtime(config.rt_priority);

        const auto period = config.cycle_time;
        auto next_wake    = steady_clock::now() + period;

        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next_wake);

            ecx_send_processdata(&ctx);
            const int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);

            if (callback) {
                callback(
                    std::span<const uint8_t>{output_buf(), output_size()},
                    std::span<const uint8_t>{input_buf(),  input_size()}
                );
            }

            // Working counter mismatch: a device may have dropped out of OP.
            if (config.recover_on_lost_link && wkc < expected_wkc()) {
                check_and_recover_devices();
            }

            cycle_count.fetch_add(1, std::memory_order_relaxed);

            const auto now = steady_clock::now();
            if (now > next_wake + period) {
                missed_cycles.fetch_add(1, std::memory_order_relaxed);
                next_wake = now + period;  // re-anchor to avoid cascading misses
            } else {
                next_wake += period;
            }
        }
    }
};

// ── Main public API ───────────────────────────────────────────────────────────

Main::Main(MainConfig config)
    : impl_(std::make_unique<Impl>())
{
    impl_->config = std::move(config);
    std::memset(&impl_->ctx, 0, sizeof(impl_->ctx));
}

Main::~Main() {
    if (impl_ && impl_->running.load()) {
        stop();
    }
}

Main::Main(Main&&) noexcept = default;
Main& Main::operator=(Main&&) noexcept = default;

int Main::init() {
    if (!ecx_init(&impl_->ctx, impl_->config.interface.c_str())) {
#ifdef __APPLE__
        throw std::runtime_error(
            "ecx_init() failed on '" + impl_->config.interface + "'. "
            "On macOS: use 'en4'/'en5' etc. (not 'eth0'). "
            "Run `networksetup -listallhardwareports` to list interfaces. "
            "Your user must be in the 'access_bpf' group — "
            "run `sudo dseditgroup -o edit -a $USER -t user access_bpf` then log out and back in."
        );
#else
        throw std::runtime_error(
            "ecx_init() failed on '" + impl_->config.interface + "'. "
            "Check the interface name and that you have CAP_NET_RAW "
            "(sudo setcap cap_net_raw+ep <binary>) or are running as root."
        );
#endif
    }

    const int found = ecx_config_init(&impl_->ctx);
    if (found <= 0) {
        // No devices on the bus — NIC opened successfully, nothing to configure.
        // Return 0; caller decides whether to proceed or wait for devices.
        impl_->populate_device_list();
        return 0;
    }

    // Apply custom PDO mappings (PreOp state, before process image is built).
    for (const auto& dcfg : impl_->config.device_pdos) {
        const auto didx = static_cast<uint16_t>(dcfg.device_index);

        // Helper: write one direction (TxPDO or RxPDO).
        auto apply_direction = [&](const std::vector<PdoConfigMapping>& mappings,
                                   uint16_t assign_obj) {
            // Clear the PDO assign list.
            uint8_t zero = 0;
            ecx_SDOwrite(&impl_->ctx, didx, assign_obj, 0, FALSE,
                         sizeof(zero), &zero, EC_TIMEOUTSTATE);

            for (std::size_t mi = 0; mi < mappings.size(); ++mi) {
                const auto& m = mappings[mi];

                // Clear the mapping object entry count.
                ecx_SDOwrite(&impl_->ctx, didx, m.pdo_index, 0, FALSE,
                             sizeof(zero), &zero, EC_TIMEOUTSTATE);

                // Write each signal entry:
                // bits 31-16: object index, 15-8: subindex, 7-0: bit length.
                for (std::size_t ei = 0; ei < m.entries.size(); ++ei) {
                    const auto& e = m.entries[ei];
                    uint32_t entry = (static_cast<uint32_t>(e.index)      << 16)
                                   | (static_cast<uint32_t>(e.subindex)   <<  8)
                                   |  static_cast<uint32_t>(e.bit_length);
                    ecx_SDOwrite(&impl_->ctx, didx, m.pdo_index,
                                 static_cast<uint8_t>(ei + 1), FALSE,
                                 sizeof(entry), &entry, EC_TIMEOUTSTATE);
                }

                // Write the entry count back.
                uint8_t ecount = static_cast<uint8_t>(m.entries.size());
                ecx_SDOwrite(&impl_->ctx, didx, m.pdo_index, 0, FALSE,
                             sizeof(ecount), &ecount, EC_TIMEOUTSTATE);

                // Add this PDO to the assign list.
                uint16_t pdo_idx = m.pdo_index;
                ecx_SDOwrite(&impl_->ctx, didx, assign_obj,
                             static_cast<uint8_t>(mi + 1), FALSE,
                             sizeof(pdo_idx), &pdo_idx, EC_TIMEOUTSTATE);
            }

            // Write the PDO assign count.
            uint8_t count = static_cast<uint8_t>(mappings.size());
            ecx_SDOwrite(&impl_->ctx, didx, assign_obj, 0, FALSE,
                         sizeof(count), &count, EC_TIMEOUTSTATE);
        };

        if (!dcfg.rx_pdos.empty()) apply_direction(dcfg.rx_pdos, 0x1C12);
        if (!dcfg.tx_pdos.empty()) apply_direction(dcfg.tx_pdos, 0x1C13);
    }

    ecx_config_map_group(&impl_->ctx, impl_->pdo_map, 0);
    ecx_configdc(&impl_->ctx);
    ecx_statecheck(&impl_->ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

    impl_->populate_device_list();
    return impl_->ctx.slavecount;
}

void Main::start(CyclicCallback cb) {
    if (impl_->running.load()) {
        throw std::logic_error("Main::start() called while already running");
    }

    impl_->callback = std::move(cb);

    // Warm up PDO exchange then request OP.
    ecx_send_processdata(&impl_->ctx);
    ecx_receive_processdata(&impl_->ctx, EC_TIMEOUTRET);

    impl_->ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&impl_->ctx, 0);

    for (int i = 0; i < 10; ++i) {
        ecx_send_processdata(&impl_->ctx);
        ecx_receive_processdata(&impl_->ctx, EC_TIMEOUTRET);
        ecx_statecheck(&impl_->ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
        if (impl_->ctx.slavelist[0].state == EC_STATE_OPERATIONAL) break;
    }

    impl_->running.store(true);
    impl_->cyclic_thread = std::thread([this] { impl_->cyclic_loop(); });
}

void Main::start_manual() {
    if (impl_->running.load()) {
        throw std::logic_error("Main::start_manual() called while already running");
    }

    // Same OP ramp-up as start() — warm up then request operational state.
    ecx_send_processdata(&impl_->ctx);
    ecx_receive_processdata(&impl_->ctx, EC_TIMEOUTRET);

    impl_->ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&impl_->ctx, 0);

    for (int i = 0; i < 10; ++i) {
        ecx_send_processdata(&impl_->ctx);
        ecx_receive_processdata(&impl_->ctx, EC_TIMEOUTRET);
        ecx_statecheck(&impl_->ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
        if (impl_->ctx.slavelist[0].state == EC_STATE_OPERATIONAL) break;
    }

    impl_->running.store(true);
    // No thread: caller drives the PDO cycle via tick_send()/tick_receive().
}

int Main::tick_receive() noexcept {
    const int wkc = ecx_receive_processdata(&impl_->ctx, EC_TIMEOUTRET);
    impl_->cycle_count.fetch_add(1, std::memory_order_relaxed);
    // Only attempt recovery when we have configured devices and the WKC drops —
    // EC_NOFRAME (-1) is expected when no PDO was sent (device_count == 0).
    if (impl_->config.recover_on_lost_link &&
        impl_->expected_wkc() > 0 &&
        wkc < impl_->expected_wkc()) {
        impl_->check_and_recover_devices();
    }
    return wkc;
}

void Main::tick_send() noexcept {
    ecx_send_processdata(&impl_->ctx);
}

int Main::expected_wkc() const noexcept {
    return impl_->expected_wkc();
}

void Main::stop() {
    if (!impl_->running.load()) return;

    impl_->running.store(false);
    if (impl_->cyclic_thread.joinable()) {
        impl_->cyclic_thread.join();
    }

    impl_->ctx.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&impl_->ctx, 0);
    ecx_close(&impl_->ctx);
}

bool Main::is_running() const noexcept { return impl_->running.load(); }
int  Main::device_count() const noexcept { return impl_->ctx.slavecount; }

DeviceInfo Main::device_info(int index) const {
    if (index < 1 || index > static_cast<int>(impl_->devices.size())) {
        throw std::out_of_range("device index out of range");
    }
    return impl_->devices[static_cast<size_t>(index - 1)];
}

const std::vector<DeviceInfo>& Main::devices() const noexcept { return impl_->devices; }

std::span<uint8_t>       Main::output_pdo() noexcept       { return {impl_->output_buf(), impl_->output_size()}; }
std::span<const uint8_t> Main::input_pdo()  const noexcept { return {impl_->input_buf(),  impl_->input_size()}; }

uint64_t Main::cycle_count()   const noexcept { return impl_->cycle_count.load(); }
uint64_t Main::missed_cycles() const noexcept { return impl_->missed_cycles.load(); }

bool Main::all_op() const noexcept {
    for (const auto& d : impl_->devices) {
        if (d.state != DeviceState::Op) return false;
    }
    return !impl_->devices.empty();
}

// ── Non-cyclic SDO access ─────────────────────────────────────────────────────

std::vector<uint8_t> Main::sdo_read(int device_index, uint16_t index,
                                     uint8_t subindex, int max_size)
{
    std::vector<uint8_t> buf(static_cast<std::size_t>(max_size), 0);
    int sz = max_size;
    const int wkc = ecx_SDOread(&impl_->ctx,
                                 static_cast<uint16_t>(device_index),
                                 index, subindex, FALSE,
                                 &sz, buf.data(), EC_TIMEOUTSTATE);
    if (wkc <= 0 || sz <= 0) return {};
    buf.resize(static_cast<std::size_t>(sz));
    return buf;
}

bool Main::sdo_write(int device_index, uint16_t index, uint8_t subindex,
                     const void* data, int size)
{
    const int wkc = ecx_SDOwrite(&impl_->ctx,
                                  static_cast<uint16_t>(device_index),
                                  index, subindex, FALSE,
                                  size, const_cast<void*>(data), EC_TIMEOUTSTATE);
    return wkc > 0;
}

// ── PDO signal introspection ──────────────────────────────────────────────────

// Read one direction of PDO mapping for a device.
//   assign_idx : 0x1C12 (RxPDO / outputs) or 0x1C13 (TxPDO / inputs)
// Returns signals in order, with bit_offset accumulated within the device's PDO.
static std::vector<PdoSignal> read_pdo_signals(ecx_contextt& ctx,
                                                int           device_index,
                                                uint16_t      assign_idx)
{
    std::vector<PdoSignal> signals;

    // Sub-index 0 = count of assigned PDOs.
    uint8_t pdo_count = 0;
    int sz = static_cast<int>(sizeof(pdo_count));
    if (ecx_SDOread(&ctx, static_cast<uint16_t>(device_index),
                    assign_idx, 0, FALSE, &sz, &pdo_count, EC_TIMEOUTSTATE) <= 0)
        return signals;

    uint32_t bit_offset = 0;

    for (uint8_t p = 1; p <= pdo_count; ++p) {
        // Each sub-index holds the mapping object index (e.g. 0x1A00).
        uint16_t pdo_idx = 0;
        sz = static_cast<int>(sizeof(pdo_idx));
        if (ecx_SDOread(&ctx, static_cast<uint16_t>(device_index),
                        assign_idx, p, FALSE, &sz, &pdo_idx, EC_TIMEOUTSTATE) <= 0)
            continue;

        // Sub-index 0 of the mapping object = number of entries.
        uint8_t entry_count = 0;
        sz = static_cast<int>(sizeof(entry_count));
        if (ecx_SDOread(&ctx, static_cast<uint16_t>(device_index),
                        pdo_idx, 0, FALSE, &sz, &entry_count, EC_TIMEOUTSTATE) <= 0)
            continue;

        for (uint8_t e = 1; e <= entry_count; ++e) {
            // Each entry: bits 31-16 = object index,
            //             bits 15-8  = sub-index,
            //             bits  7-0  = bit length.
            uint32_t mapping = 0;
            sz = static_cast<int>(sizeof(mapping));
            if (ecx_SDOread(&ctx, static_cast<uint16_t>(device_index),
                            pdo_idx, e, FALSE, &sz, &mapping, EC_TIMEOUTSTATE) <= 0)
                continue;

            PdoSignal sig;
            sig.index      = static_cast<uint16_t>((mapping >> 16) & 0xFFFF);
            sig.subindex   = static_cast<uint8_t> ((mapping >>  8) & 0x00FF);
            sig.bit_length = static_cast<uint16_t>( mapping        & 0x00FF);
            sig.bit_offset = bit_offset;

            // Format a readable name; "padding" for gap entries (index==0).
            if (sig.index == 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "pad_%u", bit_offset);
                sig.name = buf;
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "0x%04X:%02X", sig.index, sig.subindex);
                sig.name = buf;
            }

            signals.push_back(sig);
            bit_offset += sig.bit_length;
        }
    }

    return signals;
}

std::vector<PdoSignal> Main::read_input_signals(int device_index) {
    // TxPDO assign: 0x1C13
    return read_pdo_signals(impl_->ctx, device_index, 0x1C13);
}

std::vector<PdoSignal> Main::read_output_signals(int device_index) {
    // RxPDO assign: 0x1C12
    return read_pdo_signals(impl_->ctx, device_index, 0x1C12);
}

}  // namespace ethermouse
