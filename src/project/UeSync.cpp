#include "project/UeSync.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace mf {

namespace {
constexpr const char* kSyncSchema = "materialforge.ue_sync";

std::filesystem::path SyncPath(const std::filesystem::path& root) {
    return root / ".materialforge" / "ue_sync.json";
}
} // namespace

bool UeSyncStore::Load(const std::filesystem::path& projectRoot, std::string* error) {
    Clear();
    m_ProjectRoot = projectRoot;
    if (m_ProjectRoot.empty()) {
        if (error) error->clear();
        return true;
    }
    std::ifstream stream(SyncPath(m_ProjectRoot), std::ios::binary);
    if (!stream) {
        if (error) error->clear();
        return true;
    }
    try {
        nlohmann::json document;
        stream >> document;
        if (document.value("schema", std::string()) != kSyncSchema) {
            if (error) *error = "Unsupported UE sync schema.";
            return false;
        }
        const auto entries = document.find("materials");
        if (entries != document.end() && entries->is_object()) {
            for (auto it = entries->begin(); it != entries->end(); ++it) {
                if (!it.value().is_object()) continue;
                UeSyncEntry entry;
                entry.uePath = it.value().value("uePath", std::string());
                entry.state = it.value().value("state", std::string());
                entry.updatedAt = it.value().value("updatedAt", std::string());
                m_Entries[it.key()] = std::move(entry);
            }
        }
    } catch (const std::exception& exception) {
        if (error) *error = "Could not parse UE sync state: " + std::string(exception.what());
        return false;
    }
    if (error) error->clear();
    return true;
}

bool UeSyncStore::Save(std::string* error) const {
    if (m_ProjectRoot.empty()) {
        if (error) *error = "The project has no root directory.";
        return false;
    }
    const std::filesystem::path path = SyncPath(m_ProjectRoot);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "Could not create UE sync directory: " + ec.message();
        return false;
    }
    nlohmann::json document;
    document["schema"] = kSyncSchema;
    document["version"] = 1;
    document["materials"] = nlohmann::json::object();
    for (const auto& [relative, entry] : m_Entries) {
        document["materials"][relative] = {
            {"uePath", entry.uePath},
            {"state", entry.state},
            {"updatedAt", entry.updatedAt},
        };
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "Could not write UE sync state.";
        return false;
    }
    stream << document.dump(2) << "\n";
    if (!stream) {
        if (error) *error = "Failed while writing UE sync state.";
        return false;
    }
    if (error) error->clear();
    return true;
}

void UeSyncStore::Clear() {
    m_ProjectRoot.clear();
    m_Entries.clear();
}

void UeSyncStore::Set(const std::string& relativeMaterialPath,
                      const std::string& uePath, const std::string& state) {
    UeSyncEntry& entry = m_Entries[relativeMaterialPath];
    if (!uePath.empty()) entry.uePath = uePath;
    entry.state = state;
    entry.updatedAt = NowIso8601();
}

const UeSyncEntry* UeSyncStore::Find(const std::string& relativeMaterialPath) const {
    const auto it = m_Entries.find(relativeMaterialPath);
    return it == m_Entries.end() ? nullptr : &it->second;
}

bool UeSyncStore::RemapPrefix(const std::string& oldPrefix, const std::string& newPrefix,
                              std::string* error) {
    if (oldPrefix.empty()) {
        if (error) error->clear();
        return false;
    }
    bool changed = false;
    std::map<std::string, UeSyncEntry> updated;
    for (auto& [path, entry] : m_Entries) {
        if (path == oldPrefix) {
            updated[newPrefix] = entry;
            changed = true;
        } else if (path.rfind(oldPrefix + "/", 0) == 0) {
            updated[newPrefix + path.substr(oldPrefix.size())] = entry;
            changed = true;
        } else {
            updated[path] = entry;
        }
    }
    if (!changed) {
        if (error) error->clear();
        return false;
    }
    m_Entries = std::move(updated);
    if (error) error->clear();
    return true;
}

bool UeSyncStore::RemovePrefix(const std::string& prefix, std::string* error) {
    if (prefix.empty()) {
        if (error) error->clear();
        return false;
    }
    int removed = 0;
    for (auto it = m_Entries.begin(); it != m_Entries.end();) {
        const std::string& path = it->first;
        if (path == prefix || path.rfind(prefix + "/", 0) == 0) {
            it = m_Entries.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (error) error->clear();
    return removed > 0;
}

std::string UeSyncStore::NowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return stream.str();
}

} // namespace mf
