#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "native_store.hpp"

namespace kaorios {
namespace settings_detail {
using Json = nlohmann::json;

inline std::string trim(const std::string &value) {
    auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline std::optional<std::string> get(const std::map<std::string, std::string> &values, const std::string &key) {
    auto found = values.find(key);
    if (found == values.end()) return std::nullopt;
    auto value = trim(found->second);
    if (value.empty() || value == "null") return std::nullopt;
    if (value.size() > 16 * 1024 * 1024) throw std::runtime_error("Settings payload too large");
    return found->second;
}

inline Json object(const std::string &text) {
    auto value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) throw std::runtime_error("Settings JSON must be an object");
    return value;
}

inline bool boolean(const std::string &text) {
    auto value = lower(trim(text));
    if (value == "1" || value == "true") return true;
    if (value == "0" || value == "false") return false;
    throw std::runtime_error("Settings boolean is invalid");
}

inline void package(const std::string &value) {
    if (value.empty() || value.find('.') == std::string::npos || value.front() == '.' || value.back() == '.' || value.find("..") != std::string::npos ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.'; }))
        throw std::runtime_error("Settings package identifier is invalid");
}

inline std::vector<std::string> strings(const Json &value, bool packages = false) {
    if (!value.is_array()) throw std::runtime_error("Settings list is invalid");
    std::vector<std::string> out;
    for (const auto &item : value) {
        if (!item.is_string() || item.get<std::string>().empty()) throw std::runtime_error("Settings list is invalid");
        auto text = item.get<std::string>();
        if (packages) package(text);
        out.push_back(text);
    }
    return out;
}

inline void mergeObject(Json &target, const std::string &key, const Json &values) {
    if (!target.contains(key)) target[key] = Json::object();
    if (!target[key].is_object()) throw std::runtime_error("Existing settings configuration is incompatible");
    target[key].update(values);
}

inline Json pif(const std::string &raw) {
    Json incoming;
    if (trim(raw).front() == '{') {
        incoming = object(raw);
    } else {
        incoming = Json::object();
        std::istringstream lines(raw);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty() || line.front() == '#') continue;
            auto equals = line.find('=');
            if (equals == std::string::npos || equals == 0) throw std::runtime_error("Settings PIF properties are invalid");
            auto key = trim(line.substr(0, equals));
            auto value = trim(line.substr(equals + 1));
            auto comment = value.find('#');
            if (comment != std::string::npos) value = trim(value.substr(0, comment));
            if (key.empty() || value.empty()) throw std::runtime_error("Settings PIF properties are invalid");
            incoming[key] = value;
        }
    }
    for (auto it = incoming.begin(); it != incoming.end(); ++it) {
        if (it.key().empty() || !(it.value().is_string() || it.value().is_number_integer() || it.value().is_boolean()))
            throw std::runtime_error("Settings PIF field is invalid");
        if (it.key() == "enabled" && it.value().is_string()) it.value() = boolean(it.value().get<std::string>());
        for (const auto *flag : {"spoofBuild", "spoofProps", "spoofProvider", "spoofSignature"}) {
            if (it.key() == flag && it.value().is_string()) it.value() = boolean(it.value().get<std::string>()) ? 1 : 0;
        }
    }
    if (!validateNativeJson("pif", incoming.dump())) throw std::runtime_error("Settings PIF field is invalid");
    return incoming;
}

inline void copg(Json &out, const std::map<std::string, std::string> &values) {
    auto raw = get(values, "kaorios_gameprops_json");
    std::optional<bool> enabled;
    if (raw) {
        const auto input = object(*raw);
        for (auto it = input.begin(); it != input.end(); ++it)
            if (it.key() != "games" && it.key() != "enabled" && it.key() != "debug")
                throw std::runtime_error("Settings COPG semantics unsupported");
        if (!input.contains("games") || !input["games"].is_object()) throw std::runtime_error("Settings games map is invalid");
        if (input.contains("enabled") && !input["enabled"].is_boolean()) throw std::runtime_error("Settings boolean is invalid");
        if (input.contains("debug") && (!input["debug"].is_boolean() || input["debug"].get<bool>()))
            throw std::runtime_error("Settings COPG semantics unsupported");
        enabled = input.value("enabled", !input["games"].empty());
        const std::map<std::string, std::string> fields = {
            {"MODEL", "model"}, {"BRAND", "brand"}, {"MANUFACTURER", "manufacturer"},
            {"DEVICE", "device"}, {"PRODUCT", "product"}, {"FINGERPRINT", "fingerprint"}, {"RELEASE", "release"}
        };
        std::string section = out.contains("profiles") ? "profiles" :
            (out.contains("packages") && out["packages"].is_object() ? "packages" : "profiles");
        if (!out.contains(section)) out[section] = Json::object();
        if (!out[section].is_object()) throw std::runtime_error("Existing settings configuration is incompatible");
        for (auto game = input["games"].begin(); game != input["games"].end(); ++game) {
            package(game.key());
            if (!game.value().is_object()) throw std::runtime_error("Settings game profile is invalid");
            Json mapped = Json::object();
            for (auto field = game.value().begin(); field != game.value().end(); ++field) {
                auto mapping = fields.find(field.key());
                if (mapping == fields.end()) throw std::runtime_error("Settings COPG semantics unsupported");
                if (!field.value().is_string()) throw std::runtime_error("Settings game field is invalid");
                mapped[mapping->second] = field.value();
            }
            mergeObject(out[section], game.key(), mapped);
        }
    }
    auto toggle = get(values, "kaorios_spoof_gameprops");
    if (toggle) enabled = boolean(*toggle) && enabled.value_or(true);
    if (enabled) out["enabled"] = *enabled;
}

inline void hma(Json &out, const std::string &raw) {
    const auto input = object(raw);
    const std::set<std::string> allowed = {"configVersion", "scope", "templates", "settingsTemplates", "ignoredPackagesForPresets", "installSourceSpoof", "webViewProtection", "detailLog"};
    for (auto it = input.begin(); it != input.end(); ++it)
        if (!allowed.count(it.key())) throw std::runtime_error("Settings HMA semantics unsupported");
    if (input.contains("configVersion") && (!input["configVersion"].is_number_integer() || input["configVersion"] != 1))
        throw std::runtime_error("Settings HMA version unsupported");
    for (const auto *flag : {"webViewProtection", "detailLog"}) {
        if (input.contains(flag) && (!input[flag].is_boolean() || input[flag].get<bool>()))
            throw std::runtime_error("Settings HMA semantics unsupported");
    }
    for (const auto *key : {"settingsTemplates", "ignoredPackagesForPresets", "installSourceSpoof"}) {
        if (input.contains(key)) {
            bool type = std::string(key) == "settingsTemplates" ? input[key].is_object() : input[key].is_array();
            if (!type || !input[key].empty()) throw std::runtime_error("Settings HMA semantics unsupported");
        }
    }
    Json templates = input.value("templates", Json::object());
    if (!templates.is_object()) throw std::runtime_error("Settings HMA templates are invalid");
    for (const auto &entry : templates) {
        if (!entry.is_object() || entry.size() != 1 || !entry.contains("appList")) throw std::runtime_error("Settings HMA templates are invalid");
        strings(entry["appList"], true);
    }
    Json scope = input.value("scope", Json::object());
    if (!scope.is_object()) throw std::runtime_error("Settings HMA scope is invalid");
    Json mapped = Json::object();
    for (auto caller = scope.begin(); caller != scope.end(); ++caller) {
        package(caller.key());
        std::vector<std::string> targets;
        if (caller.value().is_array()) {
            targets = strings(caller.value(), true);
        } else {
            const auto &rule = caller.value();
            if (!rule.is_object() || !input.contains("webViewProtection")) throw std::runtime_error("Settings HMA semantics unsupported");
            const std::set<std::string> ruleKeys = {"useWhitelist", "excludeSystemApps", "hideInstallationSource", "hideSystemInstallationSource", "excludeTargetInstallationSource", "extraAppList", "extraOppositeAppList", "applyTemplates", "applyPresets", "applySettingTemplates", "applySettingsPresets"};
            for (auto field = rule.begin(); field != rule.end(); ++field)
                if (!ruleKeys.count(field.key())) throw std::runtime_error("Settings HMA semantics unsupported");
            for (const auto *flag : {"useWhitelist", "hideInstallationSource", "hideSystemInstallationSource", "excludeTargetInstallationSource"})
                if (rule.contains(flag) && (!rule[flag].is_boolean() || rule[flag].get<bool>())) throw std::runtime_error("Settings HMA semantics unsupported");
            if (rule.contains("excludeSystemApps") && !rule["excludeSystemApps"].is_boolean()) throw std::runtime_error("Settings HMA rule is invalid");
            for (const auto *list : {"extraOppositeAppList", "applyPresets", "applySettingTemplates", "applySettingsPresets"})
                if (rule.contains(list) && !strings(rule[list]).empty()) throw std::runtime_error("Settings HMA semantics unsupported");
            targets = strings(rule.value("extraAppList", Json::array()), true);
            for (const auto &name : strings(rule.value("applyTemplates", Json::array()))) {
                if (caller.key() == "com.google.android.gms" || caller.key() == "com.google.android.gsf" || !templates.contains(name))
                    throw std::runtime_error("Settings HMA semantics unsupported");
                auto members = strings(templates[name]["appList"], true);
                targets.insert(targets.end(), members.begin(), members.end());
            }
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        mapped[caller.key()] = targets;
    }
    if (input.contains("scope")) mergeObject(out, "scope", mapped);
    if (input.contains("configVersion")) out["configVersion"] = 1;
}

inline const std::vector<std::string> &scalarBooleans() {
    static const std::vector<std::string> keys = {
        "kaorios_spoof_photos", "kaorios_spoof_netflix", "kaorios_spoof_disney_plus",
        "kaorios_spoof_messenger_chathead", "kaorios_spoof_pixel_apps", "kaorios_spoof_meizu_music",
        "kaorios_spoof_gms", "kaorios_spoof_vending", "kaorios_spoof_gameprops",
        "kaorios_secure_flag", "kaorios_hide_app_work", "kaorios_hide_dev_work", "kaorios_secure_flag_work",
        "kaorios_keybox_enabled", "kaorios_keybox_apply_all", "kaorios_always_strong",
        "kaorios_advanced_features", "kaorios_hide_usb_charging"
    };
    return keys;
}
} // namespace settings_detail

inline std::vector<std::string> settingsKeys(const std::string &engine) {
    if (engine == "pif") return {"kaorios_pif_json"};
    if (engine == "copg") return {"kaorios_gameprops_json", "kaorios_spoof_gameprops"};
    if (engine == "hma") return {"kaorios_hma_config"};
    if (engine == "tee") return {"kaorios_keybox_xml"};
    if (engine == "kaorios") {
        auto keys = settings_detail::scalarBooleans();
        for (const auto *key : {"kaorios_keybox_active_profile", "kaorios_keybox_apply_all_mode", "kaorios_security_patch", "kaorios_manager_package"}) keys.emplace_back(key);
        return keys;
    }
    if (engine == "all") {
        std::set<std::string> keys;
        for (const auto *name : {"pif", "copg", "hma", "tee", "kaorios"}) {
            auto selected = settingsKeys(name);
            keys.insert(selected.begin(), selected.end());
        }
        return {keys.begin(), keys.end()};
    }
    throw std::runtime_error("Settings engine unsupported");
}

// Config publication and runtime acknowledgement belong to the caller, not the adapter.
inline std::optional<std::string> adaptSettings(const std::string &engine, const std::string &existingBytes,
                                               const std::map<std::string, std::string> &values) {
    using namespace settings_detail;
    auto keys = settingsKeys(engine);
    if (engine == "all") throw std::runtime_error("Settings adapter requires one engine");
    if (std::none_of(keys.begin(), keys.end(), [&](const auto &key) { return get(values, key).has_value(); })) return std::nullopt;
    if (engine == "tee") {
        auto raw = *get(values, "kaorios_keybox_xml");
        bool root = (raw.find("<KeyboxContent") != std::string::npos && raw.find("</KeyboxContent>") != std::string::npos) ||
                    (raw.find("<Keybox") != std::string::npos && raw.find("</Keybox>") != std::string::npos);
        auto key = raw.find("<Key"), end = raw.find("</Key>");
        auto privateKey = raw.find("<PrivateKey", key), privateEnd = raw.find("</PrivateKey>", key);
        auto cert = raw.find("<Certificate", key), certEnd = raw.find("</Certificate>", key);
        if (!root || key == std::string::npos || end == std::string::npos || end <= key ||
            privateKey == std::string::npos || privateEnd == std::string::npos || privateEnd <= privateKey || privateEnd > end ||
            cert == std::string::npos || certEnd == std::string::npos || certEnd <= cert || certEnd > end)
            throw std::runtime_error("Settings keybox structure is invalid");
        return raw == existingBytes ? std::nullopt : std::optional<std::string>(raw);
    }
    auto previous = object(existingBytes.empty() ? "{}" : existingBytes);
    auto out = previous;
    if (engine == "pif") out.update(pif(*get(values, "kaorios_pif_json")));
    else if (engine == "copg") copg(out, values);
    else if (engine == "hma") hma(out, *get(values, "kaorios_hma_config"));
    else if (engine == "kaorios") {
        Json mapped = Json::object();
        for (const auto &key : scalarBooleans()) if (auto value = get(values, key)) mapped[key] = boolean(*value) ? "1" : "0";
        for (const auto *key : {"kaorios_keybox_active_profile", "kaorios_keybox_apply_all_mode", "kaorios_security_patch", "kaorios_manager_package"}) {
            if (auto value = get(values, key)) {
                auto text = trim(*value);
                if (std::string(key) == "kaorios_keybox_active_profile" && text != "hw" && text != "rkp") throw std::runtime_error("Settings profile is invalid");
                if (std::string(key) == "kaorios_keybox_apply_all_mode" && text != "auto" && text != "leaf" && text != "gen") throw std::runtime_error("Settings mode is invalid");
                if (std::string(key) == "kaorios_manager_package") package(text);
                mapped[key] = text;
            }
        }
        mergeObject(out, "settingsGlobal", mapped);
        if (!out.contains("schemaVersion")) out["schemaVersion"] = 1;
    }
    if (!validateNativeJson(engine, out.dump())) throw std::runtime_error("Adapted settings configuration is invalid");
    return out == previous ? std::nullopt : std::optional<std::string>(out.dump(2) + "\n");
}
} // namespace kaorios
