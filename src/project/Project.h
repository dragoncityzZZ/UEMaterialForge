#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mf {

constexpr const char* kProjectSchemaId = "materialforge.project";
constexpr int kProjectFormatVersion = 1;

struct ProjectResource {
    std::string path;   // Relative to the project root, forward-slash separated.
    std::string kind;   // texture / model / material / json / text / file
    bool required = true;
};

struct ProjectIntegrityIssue {
    std::string path;
    std::string reason;
};

struct ProjectEntry {
    std::string name;
    std::string relativePath;
    bool directory = false;
    std::uintmax_t size = 0;
    std::string kind;
};

// A MaterialForge project is a directory with a .mfproject manifest. All paths
// stored in the manifest are relative to the manifest's directory so projects
// remain portable when the whole folder is moved.
class MaterialProject {
public:
    MaterialProject() = default;

    static std::optional<MaterialProject> Create(const std::filesystem::path& parentDirectory,
                                                  const std::string& projectName,
                                                  std::string* error = nullptr);
    static std::optional<MaterialProject> Load(const std::filesystem::path& projectFile,
                                               std::string* error = nullptr);

    bool IsOpen() const { return !m_ProjectFile.empty(); }

    bool Save(std::string* error = nullptr);
    bool RefreshIntegrity(std::string* error = nullptr);

    bool CreateFolder(const std::string& relativeParent, const std::string& folderName,
                      std::string* error = nullptr);
    bool ImportFile(const std::filesystem::path& sourceFile,
                    const std::string& relativeDirectory,
                    std::string* importedRelativePath = nullptr,
                    std::string* error = nullptr);
    bool RenameEntry(const std::string& relativePath, const std::string& newName,
                     std::string* error = nullptr);
    bool DeleteEntry(const std::string& relativePath, bool recursive,
                     std::string* error = nullptr);
    bool MoveEntry(const std::string& relativeSource,
                   const std::string& relativeTargetDirectory,
                   std::string* error = nullptr);
    bool CopyProjectFile(const std::string& relativeSource,
                  const std::string& relativeTargetDirectory,
                  std::string* importedRelativePath = nullptr,
                  std::string* error = nullptr);
    bool SetFolderColor(const std::string& relativeFolder, const std::string& color,
                        std::string* error = nullptr);
    std::string FolderColor(const std::string& relativeFolder) const;
    bool AddResource(const std::string& relativePath, const std::string& kind = "auto",
                     bool required = true);
    bool SetMainMaterial(const std::string& relativePath, std::string* error = nullptr);

    std::vector<ProjectEntry> ListDirectory(const std::string& relativeDirectory,
                                            std::string* error = nullptr) const;

    const std::string& Name() const { return m_Name; }
    const std::filesystem::path& RootPath() const { return m_RootPath; }
    const std::filesystem::path& ProjectFilePath() const { return m_ProjectFile; }
    const std::string& MainMaterialPath() const { return m_MainMaterial; }
    const std::string& CreatedAt() const { return m_CreatedAt; }
    const std::string& LastOpenedAt() const { return m_LastOpenedAt; }
    const std::vector<ProjectResource>& Resources() const { return m_Resources; }
    const std::vector<ProjectIntegrityIssue>& Issues() const { return m_Issues; }
    bool IsBroken() const { return !m_Issues.empty(); }

    std::filesystem::path ResolveRelative(const std::string& relativePath) const;
    std::filesystem::path MainMaterialAbsolutePath() const;
    std::string RelativePathOf(const std::filesystem::path& absolutePath) const;
    bool Contains(const std::filesystem::path& absolutePath) const;

    static std::string KindForPath(const std::string& path);
    static std::string SanitizeProjectName(const std::string& name);
    static bool IsValidFolderName(const std::string& name, std::string* reason = nullptr);
    static bool IsSafeRelativePath(const std::string& path);

private:
    bool WriteManifest(std::string* error);
    bool ResolveSafeDirectory(const std::string& relativeDirectory,
                              std::filesystem::path* resolved,
                              std::string* error) const;
    bool MoveEntryTo(const std::string& relativeSource,
                     const std::string& relativeTargetDirectory,
                     const std::string& newName, std::string* error = nullptr);
    void CollectIssues();
    void AddIssue(std::string path, std::string reason);

    std::filesystem::path m_RootPath;
    std::filesystem::path m_ProjectFile;
    std::string m_Name;
    std::string m_MainMaterial;
    std::string m_CreatedAt;
    std::string m_LastOpenedAt;
    std::vector<ProjectResource> m_Resources;
    std::map<std::string, std::string> m_FolderColors;
    std::vector<ProjectIntegrityIssue> m_Issues;
};

struct RecentProjectEntry {
    std::filesystem::path projectFile;
    std::string name;
    std::string lastOpened;
};

class RecentProjects {
public:
    explicit RecentProjects(std::filesystem::path storePath = {});

    void SetStorePath(std::filesystem::path storePath);
    const std::filesystem::path& StorePath() const { return m_StorePath; }

    bool Load(std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;

    void Touch(const std::filesystem::path& projectFile, const std::string& name);
    void Remove(const std::filesystem::path& projectFile);
    void Clear();
    const std::vector<RecentProjectEntry>& Entries() const { return m_Entries; }

private:
    std::filesystem::path m_StorePath;
    std::vector<RecentProjectEntry> m_Entries;
};

} // namespace mf
