#include <loom/module.h>
#include <loom/export.h>

#include <iostream>
#include <string>

struct OscilloConfig {
    std::vector<std::string> targets; // format: "moduleId/fieldName"
    int sample_rate_hz = 10;
};

struct OscilloRecipe {};
struct OscilloRuntime {};

class Oscilloscope : public loom::Module<OscilloConfig, OscilloRecipe, OscilloRuntime> {
public:
    LOOM_MODULE_HEADER("oscilloscope", "1.0.0")

    void init(const loom::InitContext& ctx) override {
        (void)ctx;
    }

    void cyclic() override {
        for (auto &t : config().targets) {
            auto pos = t.find('/');
            if (pos == std::string::npos) continue;
            std::string targetId = t.substr(0, pos);
            std::string field = t.substr(pos+1);

            auto *mod = registry_ ? registry_->findModule(targetId) : nullptr;
            if (!mod) continue;

            auto p = mod->tracePtr(loom::DataSection::Runtime, field);
            auto tn = mod->traceTypeName(loom::DataSection::Runtime, field);
            if (!p || !tn) continue;

            double v = 0.0;
            if (tryReadNumeric(*p, *tn, v)) {
                std::cout << "[osc] " << targetId << "/" << field << " = " << v << "\n";
            }
        }
    }

    void exit() override {}
    void longRunning() override {}
private:
    static bool tryReadNumeric(void* p, const std::string& type_name, double &out) {
        if (!p) return false;
        if (type_name == "double") { out = *reinterpret_cast<double*>(p); return true; }
        if (type_name == "float")  { out = *reinterpret_cast<float*>(p);  return true; }
        if (type_name == "long")   { out = static_cast<double>(*reinterpret_cast<long*>(p)); return true; }
        if (type_name == "unsigned long") { out = static_cast<double>(*reinterpret_cast<unsigned long*>(p)); return true; }
        if (type_name == "int")    { out = static_cast<double>(*reinterpret_cast<int*>(p)); return true; }
        if (type_name == "unsigned int") { out = static_cast<double>(*reinterpret_cast<unsigned int*>(p)); return true; }
        if (type_name == "short")  { out = static_cast<double>(*reinterpret_cast<short*>(p)); return true; }
        if (type_name == "unsigned short") { out = static_cast<double>(*reinterpret_cast<unsigned short*>(p)); return true; }
        if (type_name == "char")   { out = static_cast<double>(*reinterpret_cast<char*>(p)); return true; }
        if (type_name == "unsigned char") { out = static_cast<double>(*reinterpret_cast<unsigned char*>(p)); return true; }
        if (type_name == "bool")   { out = *reinterpret_cast<bool*>(p) ? 1.0 : 0.0; return true; }
        return false;
    }
};

LOOM_REGISTER_MODULE(Oscilloscope)
