/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/optional.hpp>

#include "Debug.h"
#include "JsonWrapper.h"

class DexClass;
class DexMethod;
class DexType;

/**
 * HOWTO Use Configurable
 *
 * // Derive from Configurable
 * class MyConfigurablePass : public Configurable {
 * public:
 *  // Override get_config_name to give your class a human readable name for
 * reflection std::string get_config_name() override { return
 * "MyConfigurablePass";
 *  }
 *  // Override get_config_doc to provide a docoumentation string explaining the
 *  // overall purpose of the Configurable (e.g. your pass)
 *  std::string get_config_doc() override {
 *    return "Shrink the app by doing xyz"
 *  };
 *  // Override bind_config to declare all the parameters on your Configurable
 *  void bind_config() override {
 *    // Bind the parameter named "param_name" to param_field, with a default
 *    // value of default_param_value, should the parameter be absent when
 * parsing
 *    // the config. The 4th parameter is a help string explaining the purpose
 * of the
 *    // Configurable parameter.
 *    //
 *    // bind() can bind any type that has intrinsic support (see
 *    // DEFINE_CONFIGURABLE_PRIMITIVE macros at the bottom of this file), or
 * any
 *    // type itself which derives from Configurable.
 *    bind("para_name", default_param_value, &param_field, "Help doc explaining
 * param");
 *  }
 * private:
 *  param_type_t param_field;
 * }
 */
class Configurable {
 public:
  // Binding flags
  using bindflags_t = unsigned long;
  struct bindflags {
    struct types {
      static constexpr int shift = 0;
      // mask for type bindflags
      static constexpr bindflags_t mask = {0xffL << shift};
      // error or warn on unsresolvable types
      static constexpr bindflags_t error_if_unresolvable = {0x01L << shift};
      static constexpr bindflags_t warn_if_unresolvable = {0x02L << shift};      
    };
    struct classes {
      static constexpr int shift = 2;
      // mask for class bindflags
      static constexpr bindflags_t mask = {0xffL << shift};
      // error or warn on unsresolvable classes
      static constexpr bindflags_t error_if_unresolvable = {0x01L << shift};
      static constexpr bindflags_t warn_if_unresolvable = {0x02L << shift};      
    };
    struct methods {
      static constexpr int shift = 4;
      // mask for method bindflags
      static constexpr bindflags_t mask = {0xffL << shift};
      // error or warn on unsresolvable methods
      static constexpr bindflags_t error_if_unresolvable = {0x01L << shift};
      static constexpr bindflags_t warn_if_unresolvable = {0x02L << shift};      
      // error or warn if method is not a def
      static constexpr bindflags_t error_if_not_def = {0x04L << shift};
      static constexpr bindflags_t warn_if_not_def = {0x08L << shift};      
    };
    struct optionals {
      static constexpr int shift = 6;
      // mask for optional<> bindflags
      static constexpr bindflags_t mask = {0xffL << shift};
      // any empty string will not be bound
      static constexpr bindflags_t skip_empty_string = {0x01L << shift};
    };
  };

  struct ReflectionParam;

  struct Reflection {
    std::string name;
    std::string doc;
    std::map<std::string, ReflectionParam> params;
  };

  struct ReflectionParam {
    ReflectionParam() {}

    explicit ReflectionParam(const std::string& name,
                             const std::string& doc,
                             const bool is_required,
                             const bindflags_t bindflags,
                             const std::string& primitive) {
      this->name = name;
      this->doc = doc;
      this->is_required = is_required;
      this->bindflags = bindflags;
      this->type = Type::PRIMITIVE;
      this->variant = std::make_tuple(primitive, Reflection());
    }

    explicit ReflectionParam(const std::string& name,
                             const std::string& doc,
                             const bool is_required,
                             const bindflags_t bindflags,
                             const Reflection& composite) {
      this->name = name;
      this->doc = doc;
      this->is_required = is_required;
      this->bindflags = bindflags;
      this->type = Type::COMPOSITE;
      this->variant = std::make_tuple("", composite);
    }

    enum Type {
      /**
       *  Primtives are types we support intrinsically, e.g. scalars or arrays
       * of scalars. The primitives we support are defined by
       * DEFINE_CONFIGURABLE_PRIMITIVE macros at the bottom of this file. */
      PRIMITIVE = 0,
      /**
       * Composites are types that are made up recursively of other
       * Configurables, e.g. key/value pairs. Composite types are typically ones
       * which derive from Configurable. */
      COMPOSITE = 1,
    };

    std::string name;
    std::string doc;
    bool is_required;
    bindflags_t bindflags;

    // n.b. make this a std::variant after c++17
    Type type;
    std::tuple<std::string, Reflection> variant;
  };

 public:
  virtual ~Configurable() {}

  /**
   * Returns the human readable name of this Configurable, as used in
   * reflection. */
  virtual std::string get_config_name() = 0;

  /** Returns help text explaining this Configurable's purpose. */
  virtual std::string get_config_doc() { return default_doc(); };

  /**
   * Configurables should override this function in order to declare their
   * bindings.
   *
   * bind_config is called in both reflection and configuration parsing
   * scenarios; implementations should NOT assume that the code is only called
   * in order to parse the configuration.
   *
   * Typically, you should only be calling the bind() function in bind_config().
   * If you wish to execute imperative code during the configuration parsing
   * scenario, but not the reflection scenario, then call "after_configuration"
   * in bind_config. The function supplied to after_configuration will be called
   * immediately after bind_config has been called.
   */
  virtual void bind_config() {}

  /**
   * Returns a data structure containing the schema of this Configurable. This
   * schema itself may contain nested schemas, reflecting parameters which are
   * composite.
   */
  Reflection reflect();

  /**
   * Apply the declared bindings in order to consume json at configuration
   * time. */
  void parse_config(const JsonWrapper& json);

  // Type aliases for convience
  using MapOfVectorOfStrings =
      std::unordered_map<std::string, std::vector<std::string>>;

  static constexpr const char* default_doc() { return "TODO: Document this"; }

 protected:
  /**
   * The provided function will be called immediately after bind_config() is
   * called, in the case where we're consuming a configuration (e.g. it /won't
   * be called/ in the case where we are merely reflecting.) So, you should use
   * the function to perform any non-declarative work, such as registering
   * plugins with interdex, performing any complicated validations or
   * transformations, etc. Typically speaking, avoid needing to use this
   * function. bind_config() is intended to be as declarative as possible.
   */
  void after_configuration(std::function<void()> after_configuration_fn) {
    always_assert_log(!m_after_configuration,
                      "after_configuration may only be called once");
    m_after_configuration = after_configuration_fn;
  }

  /** 
   * Default behavior for all json -> data type coercions. this template
   * handles the case for composites (e.g. all Configurables). Primitives
   * will have specializations provided in Configurable.cpp
   */
  template <typename T>
  static T as(const Json::Value& value, bindflags_t bindflags) {
    static_assert(
        std::is_base_of<Configurable, T>::value,
        "T must be a supported primitive or derive from Configurable");
    T t;
    // TODO: wrapping this in json wrapper is stupid, can we use raw
    // json::values here?)
    t.parse_config(JsonWrapper{value});
    return t;
  }

  /** 
   * Default behavior for all parameter reflections. this template
   * handles the case for composites (e.g. all Configurables). Primitives
   * will have specializations provided in Configurable.cpp
   */
  template <typename T>
  void reflect(
      std::function<void(
          const std::string& param_name,
          const std::string& param_doc,
          const bool param_is_required,
          const bindflags_t param_bindflags,
          const Configurable::ReflectionParam::Type param_type_tag,
          const std::tuple<std::string, Configurable::Reflection>& param_type)>&
          reflector,
      const std::string& param_name,
      const std::string& param_doc,
      const bool param_is_required,
      const bindflags_t param_bindflags,
      T& param) {
    static_assert(
        std::is_base_of<Configurable, T>::value,
        "T must be a supported primitive or derive from Configurable");
    reflector(param_name,
              param_doc,
              param_is_required,
              param_bindflags,
              ReflectionParam::Type::COMPOSITE,
              std::make_tuple("", param.reflect()));
  }

  template <typename T>
  void bind(const std::string& name,
            T defaultValue,
            T& dest,
            const std::string& doc = default_doc(),
            bindflags_t bindflags = 0) {
    if (m_reflecting) {
      reflect(m_reflector,
              name,
              doc,
              false /* param_is_required */,
              bindflags,
              dest);
    } else {
      parse(name, defaultValue, dest, bindflags);
    }
  }

  template <typename T>
  void bind_required(const std::string& name,
                     T& dest,
                     const std::string& doc = default_doc(),
                     bindflags_t bindflags = 0) {
    // TODO(T44504176): we could reflect the requiredness here
    if (m_reflecting) {
      reflect(m_reflector,
              name,
              doc,
              true /* param_is_required */,
              bindflags,
              dest);
    } else {
      parse_required(name, dest, bindflags);
    }
  }

  void bind(const std::string& name,
            const char* defaultValue,
            std::string& dest,
            const std::string& doc = default_doc(),
            bindflags_t bindflags = 0) {
    bind(name, std::string(defaultValue), dest, doc, bindflags);
  }

 private:
  template <typename T>
  void parse(const std::string& name,
             T defaultValue,
             T& dest,
             bindflags_t bindflags) {
    boost::optional<const Json::Value&> value = m_parser(name);
    if (value) {
      dest = Configurable::as<T>(*value, bindflags);
    } else {
      dest = defaultValue;
    }
  }

  template <typename T>
  void parse_required(const std::string& name, T& dest, bindflags_t bindflags) {
    boost::optional<const Json::Value&> value = m_parser(name);
    if (value) {
      dest = Configurable::as<T>(*value, bindflags);
    } else {
      always_assert_log(false,
                        "Missing required parameter: %s.%s",
                        get_config_name().c_str(),
                        name.c_str());
    }
  }

 private:
  std::function<void()> m_after_configuration;
  std::function<boost::optional<const Json::Value&>(const std::string& name)>
      m_parser;
  std::function<void(
      const std::string& param_name,
      const std::string& param_doc,
      const bool param_is_required,
      const Configurable::bindflags_t param_bindflags,
      const Configurable::ReflectionParam::Type param_type_tag,
      const std::tuple<std::string, Configurable::Reflection>& param_type)>
      m_reflector;
  bool m_reflecting;
};

// Specializations for primitives

#define DEFINE_CONFIGURABLE_PRIMITIVE(type)                         \
  template <>                                                       \
  type Configurable::as<type>(const Json::Value& value,             \
                              bindflags_t bindflags);               \
  template <>                                                       \
  void Configurable::reflect(                                       \
      std::function<void(                                           \
          const std::string& param_name,                            \
          const std::string& param_doc,                             \
          const bool param_is_required,                             \
          const Configurable::bindflags_t param_bindflags,          \
          const Configurable::ReflectionParam::Type param_type_tag, \
          const std::tuple<std::string, Configurable::Reflection>&  \
              param_type)>& reflector,                              \
      const std::string& param_name,                                \
      const std::string& param_doc,                                 \
      const bool param_is_required,                                 \
      const Configurable::bindflags_t param_bindflags,              \
      type& param);

DEFINE_CONFIGURABLE_PRIMITIVE(float)
DEFINE_CONFIGURABLE_PRIMITIVE(bool)
DEFINE_CONFIGURABLE_PRIMITIVE(int)
DEFINE_CONFIGURABLE_PRIMITIVE(unsigned int)
DEFINE_CONFIGURABLE_PRIMITIVE(boost::optional<int>)
DEFINE_CONFIGURABLE_PRIMITIVE(boost::optional<unsigned int>)
DEFINE_CONFIGURABLE_PRIMITIVE(long)
DEFINE_CONFIGURABLE_PRIMITIVE(unsigned long)
DEFINE_CONFIGURABLE_PRIMITIVE(boost::optional<long>)
DEFINE_CONFIGURABLE_PRIMITIVE(boost::optional<unsigned long>)
DEFINE_CONFIGURABLE_PRIMITIVE(long long)
DEFINE_CONFIGURABLE_PRIMITIVE(unsigned long long)
DEFINE_CONFIGURABLE_PRIMITIVE(DexType*)
DEFINE_CONFIGURABLE_PRIMITIVE(std::string)
DEFINE_CONFIGURABLE_PRIMITIVE(Json::Value)
DEFINE_CONFIGURABLE_PRIMITIVE(boost::optional<std::string>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::vector<std::string>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<std::string>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::vector<DexType*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<DexType*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<const DexType*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<DexClass*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<DexMethod*>)
DEFINE_CONFIGURABLE_PRIMITIVE(Configurable::MapOfVectorOfStrings)

#undef DEFINE_CONFIGURABLE_PRIMITIVE
