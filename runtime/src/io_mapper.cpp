#include <loom/io_mapper.h>
#include <loom/runtime_core.h>
#include <loom/module.h>
#include <loom/data_engine.h>
#include <loom/tag_table.hpp>
#include <glaze/glaze.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

namespace loom {

// ============================================================================
// Parsing and Resolution
// ============================================================================

IOMapper::ParsedAddress IOMapper::parseAddress(const std::string& address) {
    // Format: "module_id.section.rest" where section must be "runtime"
    // Returns {module_id, rest} where rest is the TagTable path (/ separated)
    size_t dot1 = address.find('.');
    if (dot1 == std::string::npos) {
        return {"", "", false};
    }

    std::string module_id = address.substr(0, dot1);
    size_t dot2 = address.find('.', dot1 + 1);
    if (dot2 == std::string::npos) {
        return {"", "", false};
    }

    std::string section = address.substr(dot1 + 1, dot2 - dot1 - 1);
    if (section != "runtime") {
        return {"", "", false};
    }

    // Convert remaining dots to slashes for TagTable path
    std::string rest = address.substr(dot2 + 1);
    for (char& c : rest) {
        if (c == '.') c = '/';
    }

    return {module_id, rest, true};
}

void IOMapper::resolveOne(size_t index, const RuntimeCore& runtime) {
    if (index >= entries_.size() || index >= resolved_.size()) {
        return;
    }

    IOMapEntry& entry = entries_[index];
    ResolvedMapping& res = resolved_[index];

    res.config_index = index;
    res.valid = false;
    res.error.clear();
    res.copy_fn = nullptr;

    // Parse addresses
    auto src_addr = parseAddress(entry.source);
    auto dst_addr = parseAddress(entry.target);

    if (!src_addr.valid) {
        res.error = "invalid source format: " + entry.source;
        return;
    }
    if (!dst_addr.valid) {
        res.error = "invalid target format: " + entry.target;
        return;
    }

    res.src_module_id = src_addr.module_id;
    res.dst_module_id = dst_addr.module_id;
    res.src_path = src_addr.path;
    res.dst_path = dst_addr.path;

    // Find modules
    auto* loaded_src = runtime.loader().get(src_addr.module_id);
    auto* loaded_dst = runtime.loader().get(dst_addr.module_id);
    
    if (!loaded_src || !loaded_src->instance) {
        res.error = "source module '" + src_addr.module_id + "' not found";
        return;
    }
    if (!loaded_dst || !loaded_dst->instance) {
        res.error = "destination module '" + dst_addr.module_id + "' not found";
        return;
    }

    IModule* src_mod = loaded_src->instance.get();
    IModule* dst_mod = loaded_dst->instance.get();

    res.src_mod = src_mod;
    res.dst_mod = dst_mod;

    // Resolve source pointer and type
    auto src_ptr_opt = src_mod->tracePtr(DataSection::Runtime, src_addr.path);
    auto src_type_opt = src_mod->traceTypeName(DataSection::Runtime, src_addr.path);

    if (!src_ptr_opt || !src_type_opt) {
        res.error = "field '" + src_addr.path + "' not found in source module '" + src_addr.module_id + "'";
        return;
    }

    res.src_ptr = *src_ptr_opt;
    std::string src_type = *src_type_opt;

    // Resolve destination pointer and type
    auto dst_ptr_opt = dst_mod->tracePtr(DataSection::Runtime, dst_addr.path);
    auto dst_type_opt = dst_mod->traceTypeName(DataSection::Runtime, dst_addr.path);

    if (!dst_ptr_opt || !dst_type_opt) {
        res.error = "field '" + dst_addr.path + "' not found in destination module '" + dst_addr.module_id + "'";
        return;
    }

    res.dst_ptr = *dst_ptr_opt;
    std::string dst_type = *dst_type_opt;

    // Check stability in both TagTables
    // For now, assume stable (ideally we'd check the TagTable directly, but
    // that would require API extensions). Dynamic paths will be re-resolved each tick.
    res.stable = true;  // TODO: query TagTable.is_stable()

    // Build copy function
    bool uses_json = false;
    res.copy_fn = buildCopyFn(src_type, dst_type, src_mod, dst_mod, src_addr.path, dst_addr.path, uses_json);

    if (!res.copy_fn) {
        res.error = "cannot map '" + src_type + "' to '" + dst_type + "'";
        return;
    }

    res.uses_json = uses_json;
    res.valid = true;
    res.error.clear();

    spdlog::debug("IOMapper: resolved mapping [{}] {}.runtime.{} -> {}.runtime.{}",
        index, src_addr.module_id, src_addr.path, dst_addr.module_id, dst_addr.path);
}

std::function<void(const void*, void*)> IOMapper::buildCopyFn(
    const std::string& src_type,
    const std::string& dst_type,
    IModule* src_mod,
    IModule* dst_mod,
    const std::string& src_path,
    const std::string& dst_path,
    bool& out_uses_json
) const {
    out_uses_json = false;

    // Same-type scalars: direct pointer cast
    if (src_type == dst_type) {
        // Check if it's a scalar type
        if (src_type == "double") {
            return [](const void* s, void* d) {
                *static_cast<double*>(d) = *static_cast<const double*>(s);
            };
        } else if (src_type == "float") {
            return [](const void* s, void* d) {
                *static_cast<float*>(d) = *static_cast<const float*>(s);
            };
        } else if (src_type == "long") {
            return [](const void* s, void* d) {
                *static_cast<long*>(d) = *static_cast<const long*>(s);
            };
        } else if (src_type == "unsigned long") {
            return [](const void* s, void* d) {
                *static_cast<unsigned long*>(d) = *static_cast<const unsigned long*>(s);
            };
        } else if (src_type == "int") {
            return [](const void* s, void* d) {
                *static_cast<int*>(d) = *static_cast<const int*>(s);
            };
        } else if (src_type == "unsigned int") {
            return [](const void* s, void* d) {
                *static_cast<unsigned int*>(d) = *static_cast<const unsigned int*>(s);
            };
        } else if (src_type == "short") {
            return [](const void* s, void* d) {
                *static_cast<short*>(d) = *static_cast<const short*>(s);
            };
        } else if (src_type == "unsigned short") {
            return [](const void* s, void* d) {
                *static_cast<unsigned short*>(d) = *static_cast<const unsigned short*>(s);
            };
        } else if (src_type == "char") {
            return [](const void* s, void* d) {
                *static_cast<char*>(d) = *static_cast<const char*>(s);
            };
        } else if (src_type == "unsigned char") {
            return [](const void* s, void* d) {
                *static_cast<unsigned char*>(d) = *static_cast<const unsigned char*>(s);
            };
        } else if (src_type == "bool") {
            return [](const void* s, void* d) {
                *static_cast<bool*>(d) = *static_cast<const bool*>(s);
            };
        } else {
            // Complex same-type: use JSON round-trip
            // For this we need to capture the get_json/set_json lambdas from the TagTable
            out_uses_json = true;
            return [src_mod, dst_mod, src_path, dst_path](const void* /*s*/, void* /*d*/) {
                // Read source as JSON, write to destination as JSON
                auto src_json_opt = src_mod->readField(DataSection::Runtime, src_path);
                if (src_json_opt) {
                    dst_mod->writeField(DataSection::Runtime, dst_path, *src_json_opt);
                }
            };
        }
    }

    // Cross-type scalars: implicit conversion
    if ((src_type == "int" || src_type == "float" || src_type == "double" || src_type == "bool") &&
        (dst_type == "int" || dst_type == "float" || dst_type == "double" || dst_type == "bool")) {
        
        if (src_type == "int") {
            if (dst_type == "double") {
                return [](const void* s, void* d) {
                    *static_cast<double*>(d) = static_cast<double>(*static_cast<const int*>(s));
                };
            } else if (dst_type == "float") {
                return [](const void* s, void* d) {
                    *static_cast<float*>(d) = static_cast<float>(*static_cast<const int*>(s));
                };
            } else if (dst_type == "bool") {
                return [](const void* s, void* d) {
                    *static_cast<bool*>(d) = *static_cast<const int*>(s) != 0;
                };
            }
        } else if (src_type == "float") {
            if (dst_type == "double") {
                return [](const void* s, void* d) {
                    *static_cast<double*>(d) = static_cast<double>(*static_cast<const float*>(s));
                };
            } else if (dst_type == "int") {
                return [](const void* s, void* d) {
                    *static_cast<int*>(d) = static_cast<int>(*static_cast<const float*>(s));
                };
            } else if (dst_type == "bool") {
                return [](const void* s, void* d) {
                    *static_cast<bool*>(d) = *static_cast<const float*>(s) != 0.0f;
                };
            }
        } else if (src_type == "double") {
            if (dst_type == "float") {
                return [](const void* s, void* d) {
                    *static_cast<float*>(d) = static_cast<float>(*static_cast<const double*>(s));
                };
            } else if (dst_type == "int") {
                return [](const void* s, void* d) {
                    *static_cast<int*>(d) = static_cast<int>(*static_cast<const double*>(s));
                };
            } else if (dst_type == "bool") {
                return [](const void* s, void* d) {
                    *static_cast<bool*>(d) = *static_cast<const double*>(s) != 0.0;
                };
            }
        } else if (src_type == "bool") {
            if (dst_type == "int") {
                return [](const void* s, void* d) {
                    *static_cast<int*>(d) = *static_cast<const bool*>(s) ? 1 : 0;
                };
            } else if (dst_type == "float") {
                return [](const void* s, void* d) {
                    *static_cast<float*>(d) = *static_cast<const bool*>(s) ? 1.0f : 0.0f;
                };
            } else if (dst_type == "double") {
                return [](const void* s, void* d) {
                    *static_cast<double*>(d) = *static_cast<const bool*>(s) ? 1.0 : 0.0;
                };
            }
        }
    }

    // No compatible conversion
    return nullptr;
}

void IOMapper::executeMapping(const ResolvedMapping& mapping) {
    if (!mapping.valid || !mapping.copy_fn) {
        return;
    }

    void* src_ptr = mapping.src_ptr;
    void* dst_ptr = mapping.dst_ptr;

    // If mapping is dynamic, re-resolve pointers each tick
    if (!mapping.stable) {
        auto src_ptr_opt = mapping.src_mod->tracePtr(DataSection::Runtime, mapping.src_path);
        auto dst_ptr_opt = mapping.dst_mod->tracePtr(DataSection::Runtime, mapping.dst_path);
        if (!src_ptr_opt || !dst_ptr_opt) {
            return;  // Field disappeared, skip this tick
        }
        src_ptr = *src_ptr_opt;
        dst_ptr = *dst_ptr_opt;
    }

    // If cross-class read, acquire shared_lock on source
    if (mapping.src_mod != mapping.dst_mod && mapping.src_mod && mapping.src_mod->runtimeLock()) {
        std::shared_lock lock(*mapping.src_mod->runtimeLock());
        mapping.copy_fn(src_ptr, dst_ptr);
    } else {
        mapping.copy_fn(src_ptr, dst_ptr);
    }
}

// ============================================================================
// Public API
// ============================================================================

bool IOMapper::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file) {
        spdlog::warn("IOMapper: file not found: {}", filepath);
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<IOMapEntry> loaded;
    auto ec = glz::read_json(loaded, content);
    if (ec) {
        spdlog::error("IOMapper: failed to parse {}: {}", filepath, glz::format_error(ec, ""));
        return false;
    }

    entries_ = std::move(loaded);
    resolved_.resize(entries_.size());

    spdlog::info("IOMapper: loaded {} mappings from {}", entries_.size(), filepath);
    return true;
}

bool IOMapper::save(const std::string& filepath) const {
    std::ofstream file(filepath);
    if (!file) {
        spdlog::error("IOMapper: cannot create file: {}", filepath);
        return false;
    }

    auto str = glz::write_json(entries_).value_or("");
    file << str;
    if (!file) {
        spdlog::error("IOMapper: failed to write {}", filepath);
        return false;
    }

    spdlog::info("IOMapper: saved {} mappings to {}", entries_.size(), filepath);
    return true;
}

void IOMapper::resolveAll(const RuntimeCore& runtime) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        resolveOne(i, runtime);
    }
}

void IOMapper::invalidateModule(const std::string& module_id) {
    for (auto& res : resolved_) {
        if (res.src_module_id == module_id || res.dst_module_id == module_id) {
            res.valid = false;
            res.error = "module reloaded";
        }
    }
}

void IOMapper::executeForClass(const std::string& class_name) {
    // Get all modules in this class from the scheduler configuration
    // For now, execute all mappings whose destination is in this class.
    // This is a simplified approach; a full implementation would track module-to-class membership.
    for (size_t i = 0; i < resolved_.size(); ++i) {
        const auto& res = resolved_[i];
        if (i >= entries_.size() || !res.valid || !entries_[i].enabled) {
            continue;
        }
        // TODO: check if res.dst_mod is in class_name
        executeMapping(res);
    }
}

void IOMapper::executeForModule(const std::string& module_id) {
    for (size_t i = 0; i < resolved_.size(); ++i) {
        const auto& res = resolved_[i];
        if (i >= entries_.size() || !res.valid || !entries_[i].enabled) {
            continue;
        }
        if (res.dst_module_id == module_id) {
            executeMapping(res);
        }
    }
}

size_t IOMapper::addMapping(const std::string& source, const std::string& target, bool enabled) {
    entries_.push_back({source, target, enabled});
    resolved_.push_back({});
    return entries_.size() - 1;
}

bool IOMapper::updateMapping(size_t index, const std::string& source, const std::string& target, bool enabled) {
    if (index >= entries_.size()) {
        return false;
    }
    entries_[index] = {source, target, enabled};
    resolved_[index] = {};
    return true;
}

bool IOMapper::removeMapping(size_t index) {
    if (index >= entries_.size()) {
        return false;
    }
    entries_.erase(entries_.begin() + index);
    resolved_.erase(resolved_.begin() + index);
    return true;
}

bool IOMapper::setEnabled(size_t index, bool enabled) {
    if (index >= entries_.size()) {
        return false;
    }
    entries_[index].enabled = enabled;
    return true;
}

const ResolvedMapping* IOMapper::getMapping(size_t index) const {
    if (index >= resolved_.size()) {
        return nullptr;
    }
    return &resolved_[index];
}

} // namespace loom
