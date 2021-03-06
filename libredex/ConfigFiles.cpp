/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConfigFiles.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "Debug.h"
#include "DexClass.h"

void JsonWrapper::get(const char* name, int64_t dflt, int64_t& param) const {
  param = m_config.get(name, (Json::Int64)dflt).asInt();
}

void JsonWrapper::get(const char* name, size_t dflt, size_t& param) const {
  param = m_config.get(name, (Json::UInt)dflt).asUInt();
}

void JsonWrapper::get(const char* name,
                     const std::string& dflt,
                     std::string& param) const {
  param = m_config.get(name, dflt).asString();
}

void JsonWrapper::get(const char* name, bool dflt, bool& param) const {
  auto val = m_config.get(name, dflt);

  // Do some simple type conversions that folly used to do
  if (val.isBool()) {
    param = val.asBool();
    return;
  } else if (val.isInt()) {
    auto valInt = val.asInt();
    if (valInt == 0 || valInt == 1) {
      param = (val.asInt() != 0);
      return;
    }
  } else if (val.isString()) {
    auto str = val.asString();
    std::transform(str.begin(), str.end(), str.begin(),
                   [](auto c) { return ::tolower(c); });
    if (str == "0" || str == "false" || str == "off" || str == "no") {
      param = false;
      return;
    } else if (str == "1" || str == "true" || str == "on" || str == "yes") {
      param = true;
      return;
    }
  }
  throw std::runtime_error("Cannot convert JSON value to bool: " +
                           val.asString());
}

void JsonWrapper::get(const char* name,
                     const std::vector<std::string>& dflt,
                     std::vector<std::string>& param) const {
  auto it = m_config[name];
  if (it == Json::nullValue) {
    param = dflt;
  } else {
    param.clear();
    for (auto const& str : it) {
      param.emplace_back(str.asString());
    }
  }
}

void JsonWrapper::get(const char* name,
                     const std::vector<std::string>& dflt,
                     std::unordered_set<std::string>& param) const {
  auto it = m_config[name];
  param.clear();
  if (it == Json::nullValue) {
    param.insert(dflt.begin(), dflt.end());
  } else {
    for (auto const& str : it) {
      param.emplace(str.asString());
    }
  }
}

void JsonWrapper::get(
    const char* name,
    const std::unordered_map<std::string, std::vector<std::string>>& dflt,
    std::unordered_map<std::string, std::vector<std::string>>& param) const {
  auto cfg = m_config[name];
  param.clear();
  if (cfg == Json::nullValue) {
    param = dflt;
  } else {
    if (!cfg.isObject()) {
      throw std::runtime_error("Cannot convert JSON value to object: " +
                               cfg.asString());
    }
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
      auto key = it.key();
      if (!key.isString()) {
        throw std::runtime_error("Cannot convert JSON value to string: " +
                                 key.asString());
      }
      auto& val = *it;
      if (!val.isArray()) {
        throw std::runtime_error("Cannot convert JSON value to array: " +
                                 val.asString());
      }
      for (auto& str : val) {
        if (!str.isString()) {
          throw std::runtime_error("Cannot convert JSON value to string: " +
                                   str.asString());
        }
        param[key.asString()].push_back(str.asString());
      }
    }
  }
}

void JsonWrapper::get(const char* name,
                     const Json::Value dflt,
                     Json::Value& param) const {
  param = m_config.get(name, dflt);
}

const Json::Value& JsonWrapper::operator[](const char* name) const {
  return m_config[name];
}

ConfigFiles::ConfigFiles(const Json::Value& config, const std::string& outdir)
    : m_json(config),
      outdir(outdir),
      m_proguard_map(config.get("proguard_map", "").asString()),
      m_coldstart_class_filename(
          config.get("coldstart_classes", "").asString()),
      m_coldstart_method_filename(
          config.get("coldstart_methods", "").asString()),
      m_printseeds(config.get("printseeds", "").asString()) {
  auto no_optimizations_anno = config["no_optimizations_annotations"];
  if (no_optimizations_anno != Json::nullValue) {
    for (auto const& config_anno_name : no_optimizations_anno) {
      std::string anno_name = config_anno_name.asString();
      DexType* anno = DexType::get_type(anno_name.c_str());
      if (anno) m_no_optimizations_annos.insert(anno);
    }
  }
}

ConfigFiles::ConfigFiles(const Json::Value& config) : ConfigFiles(config, "") {}

/**
 * Read an interdex list file and return as a vector of appropriately-formatted
 * classname strings.
 */
std::vector<std::string> ConfigFiles::load_coldstart_classes() {
  const char* kClassTail = ".class";
  const size_t lentail = strlen(kClassTail);
  auto file = m_coldstart_class_filename.c_str();

  std::vector<std::string> coldstart_classes;

  std::ifstream input(file);
  if (!input){
    return std::vector<std::string>();
  }
  std::string clzname;
  while (input >> clzname) {
		long position = clzname.length() - lentail;
    always_assert_log(position >= 0,
                      "Bailing, invalid class spec '%s' in interdex file %s\n",
                      clzname.c_str(), file);
    clzname.replace(position, lentail, ";");
    coldstart_classes.emplace_back(m_proguard_map.translate_class("L" + clzname));
  }
  return coldstart_classes;
}

/*
 * Read the method list file and return it is a vector of strings.
 */
std::vector<std::string> ConfigFiles::load_coldstart_methods() {
  std::ifstream listfile(m_coldstart_method_filename);
  if (!listfile) {
    fprintf(stderr, "Failed to open coldstart method list: `%s'\n",
            m_coldstart_method_filename.c_str());
    return std::vector<std::string>();
  }
  std::vector<std::string> coldstart_methods;
  std::string method;
  while (std::getline(listfile, method)) {
    if (method.length() > 0) {
      coldstart_methods.push_back(m_proguard_map.translate_method(method));
    }
  }
  return coldstart_methods;
}
