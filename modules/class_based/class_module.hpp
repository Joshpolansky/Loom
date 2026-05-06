#include <glaze/core/reflect.hpp>

class ConfigurationBase{
    public:
    int x = 0;
    ConfigurationBase(){
        x = 42;
    };
    ~ConfigurationBase() = default;
    virtual void reset(){
        x = 42;
    };
};

class Configuration : public ConfigurationBase{
    public:
    int a = 0;
    Configuration(){
        a = 24;
    };
    ~Configuration() = default;
    virtual void reset() override {
        ConfigurationBase::reset();
        a = 24;
    };
};

template <>
struct glz::meta<Configuration> {
     using T = Configuration;
     static constexpr auto value = object(&T::x, &T::a);
};      

class Runtime{
    public:
    int y = 0;
    Runtime(){
        y = 100;
    };
    ~Runtime() = default;
};

template <>
struct glz::meta<Runtime> {
     using T = Runtime;
     static constexpr auto value = object(&T::y);
};      

class Recipe{
    public:
    int z = 0;
    Recipe(){
        z = 200;
    };
    ~Recipe() = default;
};


template <>
struct glz::meta<Recipe> {
     using T = Recipe;
     static constexpr auto value = object(&T::z);
};      
