#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
// Async commands — the third inter-module primitive (alongside topics & RPC)
//
// Topics are fire-and-forget; services are synchronous request/response. Neither
// fits a long-running, stateful operation that needs progress + Done/Aborted/
// Error feedback (a motion, a heater ramp, a pump sequence). Commands fill that
// gap, with PLCopen-style status delivered by write-through — no polling.
//
// Flow:
//   - A consumer holds a std::shared_ptr<CommandStatus> (from the runtime heap)
//     and submits {command, target, params, weak_ptr<status>} to a provider's
//     CommandChannel (looked up on the Bus by provider id).
//   - The provider drains its channel in cyclic(), runs the work over as many
//     cycles as needed, and writes status THROUGH the weak_ptr. If the consumer
//     has gone, weak_ptr::lock() fails and the provider drops the report.
//
// This header is standalone (no <loom/module.h>); CommandClient / CommandFb,
// which need a module handle, live in command_client.h.
// ============================================================================

namespace loom {

/// Lifecycle phase of a single async command. Mirrors PLCopen FB outputs and
/// the ATN IDLE / EXECUTE / WAITING / DONE / ABORT / ERROR / BYPASSED set.
enum class CmdPhase : uint8_t {
    None     = 0,
    Queued   = 1,  ///< accepted, waiting behind another command (Buffered)
    Active   = 2,  ///< currently controlling the provider
    Done     = 3,  ///< terminal: success
    Aborted  = 4,  ///< terminal: superseded / cancelled
    Error    = 5,  ///< terminal: rejected / faulted (see error_id)
    Bypassed = 6,  ///< provider bypassed (e.g. simulation)
};

/// Status of one async command, written through by the provider and read by the
/// issuing function block. The provider (its cyclic thread) and the consumer FB
/// may run on different scheduler threads, so the fields are atomic to avoid a
/// data race; write with .store(), read with .load(). Still trivially
/// destructible (atomics of trivial types are), so it lives in runtime-heap
/// storage (see runtime_heap.h). Note: per-field atomics give no cross-field
/// snapshot — a reader re-reads each cycle and converges; map correlated state
/// onto `phase` (one load) where consistency matters.
struct CommandStatus {
    std::atomic<CmdPhase> phase    {CmdPhase::None};
    std::atomic<bool>     busy     {false};
    std::atomic<bool>     active   {false};
    std::atomic<bool>     done     {false};
    std::atomic<bool>     aborted  {false};
    std::atomic<bool>     error    {false};
    std::atomic<uint32_t> error_id {0};
    std::atomic<double>   progress {0.0};  ///< optional 0..1 progress for long commands
};

/// PLCopen buffer behaviour at the provider when a command arrives.
enum class BufferMode : uint8_t {
    Aborting = 0,  ///< immediately supersede the active command
    Buffered = 1,  ///< queue behind the active command
};

/// FB-side error id used when a submission can't be delivered.
inline constexpr uint32_t kErrSubmitFailed = 0xA001;

/// One submitted command. Fixed layout, so its ABI does not grow as a provider's
/// command set grows. `command` is a *provider-defined* id — each provider
/// declares its own `enum class : uint32_t` of commands and casts it here; the
/// generic layer treats it as an opaque integer (no string parsing on dispatch).
/// `params` carries the per-command payload (JSON by convention) and is opaque.
struct CommandSubmission {
    uint32_t    command     = 0;    ///< provider-defined command id (its enum, as int)
    uint32_t    target      = 0;    ///< sub-entity index (e.g. axis); 0 if N/A
    std::string params;             ///< opaque payload (JSON by convention)
    BufferMode  buffer_mode = BufferMode::Aborting;
    std::weak_ptr<CommandStatus> status;  ///< issuer-owned cell (runtime heap)
};

/// Provider-owned submission inbox. Registered on the Bus under the provider's
/// id; the provider drains it once per cycle. Thread-safe (submit from consumer
/// threads, drain from the provider's cyclic thread).
class CommandChannel {
public:
    void submit(CommandSubmission s) {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_.push_back(std::move(s));
    }
    void drain(std::vector<CommandSubmission>& out) {
        out.clear();
        std::lock_guard<std::mutex> lk(mutex_);
        out.swap(pending_);
    }

private:
    std::mutex                     mutex_;
    std::vector<CommandSubmission> pending_;
};

/// Reusable edge-triggered function-block base for PLCopen-style commands. A
/// domain FB sets `execute`, on a rising edge builds its params + submits via a
/// CommandClient (see command_client.h), and passes the result to commit();
/// outputs then mirror the status cell. Depends only on CommandStatus — no
/// module handle, no serialization — so it stays in this glaze-free header.
class CommandFb {
public:
    bool       execute     = false;
    BufferMode buffer_mode = BufferMode::Aborting;

    bool     busy            = false;
    bool     active          = false;
    bool     done            = false;
    bool     command_aborted = false;
    bool     error           = false;
    uint32_t error_id        = 0;

protected:
    /// True on the Execute rising edge (valid before commit() updates state).
    bool rising() const { return execute && !prev_execute_; }

    /// Apply one cycle's outcome. On a rising edge `submitted` is the submit
    /// result (possibly null on failure); otherwise pass nullptr.
    ///
    /// PLCopen output rules enforced here:
    ///  - exactly one of {busy, done, command_aborted, error} is set at a time
    ///    (active may accompany busy);
    ///  - dropping Execute does NOT cancel — the block stays Busy until the
    ///    command finishes, then surfaces the terminal result;
    ///  - a terminal result (Done/Aborted/Error) is held while Execute is high,
    ///    and is visible for >= 1 cycle even if Execute is already low;
    ///  - a rising Execute starts a new command, superseding any held result.
    void commit(bool rising_edge, std::shared_ptr<CommandStatus> submitted) {
        // A held terminal result clears only on a LATER call with Execute low, so
        // it was visible for at least the cycle it was set in.
        if (state_ == State::Held && !execute) {
            clearOutputs();
            status_.reset();
            state_ = State::Idle;
        }

        if (rising_edge) {  // new command supersedes anything currently shown
            clearOutputs();
            status_ = std::move(submitted);
            if (!status_) { error = true; error_id = kErrSubmitFailed; state_ = State::Held; }
            else          { busy = true;  state_ = State::Running; }
        }

        if (state_ == State::Running && status_) {
            const CmdPhase ph = status_->phase.load();
            if (ph == CmdPhase::Done || ph == CmdPhase::Aborted || ph == CmdPhase::Error) {
                clearOutputs();
                done            = (ph == CmdPhase::Done);
                command_aborted = (ph == CmdPhase::Aborted);
                error           = (ph == CmdPhase::Error);
                error_id        = status_->error_id.load();
                state_          = State::Held;
            } else {              // still in flight (Queued / Active / None)
                busy   = true;
                active = (ph == CmdPhase::Active);
            }
        }

        prev_execute_ = execute;
    }

private:
    enum class State { Idle, Running, Held };
    void clearOutputs() {
        busy = active = done = command_aborted = error = false;
        error_id = 0;
    }
    std::shared_ptr<CommandStatus> status_;
    bool  prev_execute_ = false;
    State state_        = State::Idle;
};

} // namespace loom
