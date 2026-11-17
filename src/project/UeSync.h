#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace mf {

struct UeSyncEntry {
    std::string uePath;
    std::string state;      // exported / pushed / pulled
    std::string updatedAt;
};

// Per-project UE round-trip metadata. Stored in .materialforge/ue_sync.json and
// keyed by project-relative material path.
class UeSyncStore {
public:
    bool Load(const std::filesystem::path& projectRoot, std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;
    void Clear();

    void Set(const std::string& relativeMaterialPath, const std::string& uePath,
             const std::string& state);
    const UeSyncEntry* Find(const std::string& relativeMaterialPath) const;
    // Keeps sync records attached when a file or folder is renamed/moved.
    // Returns true when at least one record changed.
    bool RemapPrefix(const std::string& oldPrefix, const std::string& newPrefix,
                     std::string* error = nullptr);
    // Drops sync records for a deleted file or folder.
    // Returns true when at least one record changed.
    bool RemovePrefix(const std::string& prefix, std::string* error = nullptr);
    const std::map<std::string, UeSyncEntry>& Entries() const { return m_Entries; }

    static std::string NowIso8601();

private:
    std::filesystem::path m_ProjectRoot;
    std::map<std::string, UeSyncEntry> m_Entries;
};

} // namespace mf
