#include "ethercat.hpp"

#include <loom/module.h>
#include <loom/export.h>

#include "main.hpp"
#include "device.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// ── SDO service types ─────────────────────────────────────────────────────────

struct SdoReadRequest {
    int      device   = 1;
    uint16_t index    = 0;
    uint8_t  subindex = 0;
    int      size     = 4;   // expected payload bytes
};

struct SdoReadResponse {
    bool                 ok    = false;
    std::vector<uint8_t> data  = {};
    std::string          error = {};
};

struct SdoWriteRequest {
    int                  device   = 1;
    uint16_t             index    = 0;
    uint8_t              subindex = 0;
    std::vector<uint8_t> data     = {};
};

// ── Module ────────────────────────────────────────────────────────────────────

class EtherCatModule
    : public loom::Module<EtherCatConfig, EtherCatRecipe, EtherCatRuntime>
{
public:
    LOOM_MODULE_HEADER("EtherCat", "1.0.0")

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void init(const loom::InitContext& /*ctx*/) override {
        runtime_.enabled = true;  // allow cyclic() to exchange PDOs once main is built
        buildMain();

        registerLocalService("start", [this](std::string_view) -> loom::CallResult {
            return startMain();
        });
        registerLocalService("stop", [this](std::string_view) -> loom::CallResult {
            stopMain();
            return {true, R"({"stopped":true})", ""};
        });
        registerLocalService("rescan", [this](std::string_view) -> loom::CallResult {
            stopMain();
            buildMain();
            return startMain();
        });

        // Non-cyclic SDO read: {"device":1,"index":24641,"subindex":0,"size":2}
        // Returns:             {"ok":true,"data":[55,2],"error":""}
        registerLocalService("sdo_read", [this](const SdoReadRequest& req) -> loom::CallResult {
            if (!main_ || !main_->is_running()) {
                return {false, "", "main controller not running"};
            }

            const auto data = main_->sdo_read(req.device, req.index, req.subindex, req.size);
            SdoReadResponse resp{
                .ok    = !data.empty(),
                .data  = data,
                .error = data.empty() ? "SDO read failed or no data" : std::string{},
            };
            return {resp.ok, glz::write_json(resp).value_or("{}"),
                    resp.ok ? "" : resp.error};
        });

        // Non-cyclic SDO write: {"device":1,"index":24640,"subindex":0,"data":[15,0]}
        // Returns:              {"ok":true} or {"ok":false}
        registerLocalService("sdo_write", [this](const SdoWriteRequest& req) -> loom::CallResult {
            if (!main_ || !main_->is_running())
                return {false, "", "main controller not running"};
            const bool ok = main_->sdo_write(req.device, req.index, req.subindex,
                                              req.data.data(),
                                              static_cast<int>(req.data.size()));
            return {ok, ok ? R"({"ok":true})" : R"({"ok":false})",
                    ok ? "" : "SDO write failed"};
        });

        if (config_.auto_start) {
            startMain();
        }
    }

    // ── PDO cycle — keep everything here allocation-free ─────────────────────

    // Receive PDO input frame; copy into runtime_.inputs for cyclic() to read.
    void preCyclic() override {
        if (!main_ || !main_->is_running()) return;

        const int wkc = main_->tick_receive();

        runtime_.working_counter = wkc;
        runtime_.expected_wkc    = main_->expected_wkc();
        runtime_.wkc_ok          = (wkc >= runtime_.expected_wkc);

        const auto in = main_->input_pdo();
        runtime_.inputs.assign(in.begin(), in.end());
    }

    // Hot path: only update scalar fields. No allocations, no serialization.
    void cyclic() override {
        if (!main_) return;

        runtime_.running      = main_->is_running();
        runtime_.all_op       = main_->all_op();
        runtime_.device_count = main_->device_count();
        runtime_.cycle_count  = main_->cycle_count();

        if (!runtime_.running) {
            runtime_.status = runtime_.last_error.empty() ? "idle" : "error";
        } else if (!runtime_.wkc_ok) {
            runtime_.status = "degraded";
        } else {
            runtime_.status = "running";
        }

        // Size outputs to match PDO region once (after first device scan).
        const auto out_pdo = main_->output_pdo();
        if (runtime_.outputs.size() != out_pdo.size()) {
            runtime_.outputs.resize(out_pdo.size(), 0);
        }
    }

    // Flush runtime_.outputs to the SOEM output buffer and send the PDO frame.
    void postCyclic() override {
        if (!main_ || !main_->is_running()) return;

        auto out_pdo = main_->output_pdo();
        const std::size_t n = std::min(runtime_.outputs.size(), out_pdo.size());
        std::copy_n(runtime_.outputs.begin(), n, out_pdo.begin());

        main_->tick_send();
    }

    void exit() override {
        runtime_.enabled = false;
        stopMain();
        main_.reset();
    }

    // Publish diagnostics at ~10 Hz — device list rebuild and JSON serialization
    // belong here, not in the 1 kHz cyclic loop.
    void longRunning() override {
        while (runtime_.enabled) {
            // ── Rebuild device list when the count changes ────────────────────
            // This also triggers CoE SDO reads for the PDO signal map.
            const int current_count = main_ ? main_->device_count() : 0;
            if (current_count != last_published_device_count_) {
                last_published_device_count_ = current_count;
                runtime_.devices.clear();
                runtime_.devices.reserve(static_cast<std::size_t>(current_count));

                if (main_) {
                    for (const auto& di : main_->devices()) {
                        DeviceStatus ds;
                        ds.index         = di.index;
                        ds.name          = di.name;
                        ds.vendor_id     = di.vendor_id;
                        ds.product_code  = di.product_code;
                        ds.revision      = di.revision_number;
                        ds.in_op         = (di.state == ethermouse::DeviceState::Op);
                        ds.input_bytes   = di.input_bytes;
                        ds.output_bytes  = di.output_bytes;
                        ds.input_offset  = di.input_offset;
                        ds.output_offset = di.output_offset;

                        // CoE SDO reads — slow, but only on device-list change.
                        ds.input_signals  = main_->read_input_signals(di.index);
                        ds.output_signals = main_->read_output_signals(di.index);

                        runtime_.devices.push_back(std::move(ds));
                    }
                }
            }

            // ── Update per-device byte slices from the flat PDO buffers ───────
            // runtime_.inputs is kept fresh by preCyclic() at 1 kHz;
            // we copy slices here at 10 Hz for diagnostics/display.
            for (auto& ds : runtime_.devices) {
                if (ds.input_bytes > 0 &&
                    ds.input_offset + ds.input_bytes <= runtime_.inputs.size()) {
                    ds.inputs.assign(
                        runtime_.inputs.begin() + static_cast<std::ptrdiff_t>(ds.input_offset),
                        runtime_.inputs.begin() + static_cast<std::ptrdiff_t>(ds.input_offset + ds.input_bytes));
                }
                if (ds.output_bytes > 0 &&
                    ds.output_offset + ds.output_bytes <= runtime_.outputs.size()) {
                    ds.outputs.assign(
                        runtime_.outputs.begin() + static_cast<std::ptrdiff_t>(ds.output_offset),
                        runtime_.outputs.begin() + static_cast<std::ptrdiff_t>(ds.output_offset + ds.output_bytes));
                }
            }

            std::this_thread::sleep_for(100ms);  // 10 Hz diagnostics
        }
    }

private:
    std::unique_ptr<ethermouse::Main> main_;
    int last_published_device_count_ = -1;

    void buildMain() {
        ethermouse::MainConfig mc;
        mc.interface            = config_.interface;
        mc.recover_on_lost_link = config_.recover_on_lost_link;
        mc.device_pdos          = config_.device_configs;
        main_ = std::make_unique<ethermouse::Main>(std::move(mc));
    }

    loom::CallResult startMain() {
        runtime_.status     = "starting";
        runtime_.last_error = "";
        try {
            const int found = main_->init();
            main_->start_manual();
            runtime_.device_count = found;
            return {true, R"({"started":true,"devices":)" + std::to_string(found) + "}", ""};
        } catch (const std::exception& ex) {
            runtime_.status     = "error";
            runtime_.last_error = ex.what();
            return {false, "", ex.what()};
        }
    }

    void stopMain() {
        if (main_ && main_->is_running()) {
            main_->stop();
        }
        runtime_.status       = "idle";
        runtime_.running      = false;
        runtime_.all_op       = false;
        runtime_.device_count = 0;
        runtime_.devices.clear();
        runtime_.inputs.clear();
        runtime_.outputs.clear();
        last_published_device_count_ = -1;
    }
};

LOOM_REGISTER_MODULE(EtherCatModule)
