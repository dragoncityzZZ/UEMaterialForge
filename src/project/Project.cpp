#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
using nlohmann::json;

namespace mf {
namespace {

constexpr const char* kRecentProjectsSchema = "materialforge.recent-projects";
constexpr std::size_t kMaxRecentProjects = 10;

std::string FilesystemErrorText(const std::error_code& error) {
    switch (error.value()) {
    case 2:
        return "File not found.";
    case 3:
        return "Path not found.";
    case 5:
        return "Access denied. The file may be open or read-only, or the project may be outside MaterialForge's writable workspace. Run MaterialForge directly or move the project to a writable folder.";
    case 32:
        return "The file is being used by another process.";
    case 145:
        return "The folder is not empty.";
    default:
        return "Filesystem error " + std::to_string(error.value()) + ".";
    }
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string NowIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

std::string PathToUtf8(const fs::path& path) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
#else
    return path.u8string();
#endif
}

fs::path Utf8ToPath(const std::string& path) {
    return fs::u8path(path);
}

fs::path NormalizeAbsolute(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    const fs::path canonical = fs::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

std::string NormalizeRelativeSeparators(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 2 && path[0] == '.' && path[1] == '/') {
        path.erase(0, 2);
    }
    return path;
}

bool IsWindowsReservedName(const std::string& name) {
    const std::string upper = ToLower(name);
    std::string base = upper;
    const auto dot = base.find('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    static const std::set<std::string> reserved = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
        "lpt6", "lpt7", "lpt8", "lpt9"
    };
    return reserved.find(base) != reserved.end();
}

bool IsValidFileComponent(const std::string& raw, std::string* cleaned, std::string* reason) {
    const std::string name = Trim(raw);
    if (name.empty()) {
        if (reason) *reason = "Name cannot be empty.";
        return false;
    }
    if (name == "." || name == "..") {
        if (reason) *reason = "'.' and '..' are reserved names.";
        return false;
    }
    if (name.back() == '.' || name.back() == ' ') {
        if (reason) *reason = "Names cannot end with a dot or space on Windows.";
        return false;
    }
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            if (reason) *reason = "Name contains a character that is not allowed in file names.";
            return false;
        }
    }
    if (IsWindowsReservedName(name)) {
        if (reason) *reason = "Name is reserved by Windows.";
        return false;
    }
    if (cleaned) *cleaned = name;
    return true;
}

std::string ReadTextFile(const fs::path& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open file: " + PathToUtf8(path);
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (error) error->clear();
    return text;
}

bool WriteTextAtomically(const fs::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error) *error = "Could not create directory: " + ec.message();
            return false;
        }
    }
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) *error = "Could not write file: " + PathToUtf8(temporary);
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            if (error) *error = "Failed while writing file: " + PathToUtf8(temporary);
            return false;
        }
    }

    fs::rename(temporary, path, ec);
    if (ec) {
        std::error_code removeError;
        fs::remove(path, removeError);
        ec.clear();
        fs::rename(temporary, path, ec);
    }
    if (ec) {
        fs::remove(temporary, ec);
        if (error) *error = "Could not finalize file: " + PathToUtf8(path) + ": " + ec.message();
        return false;
    }
    if (error) error->clear();
    return true;
}

std::string JsonValueToString(const json& object, const char* key, const std::string& fallback = {}) {
    if (!object.contains(key) || !object[key].is_string()) return fallback;
    return object[key].get<std::string>();
}

bool PathsEqual(const fs::path& a, const fs::path& b) {
#if defined(_WIN32)
    return ToLower(a.generic_u8string()) == ToLower(b.generic_u8string());
#else
    return a.generic_u8string() == b.generic_u8string();
#endif
}

std::string JoinRelative(const std::string& base, const std::string& name) {
    if (base.empty()) return name;
    return base + "/" + name;
}

} // namespace

std::string MaterialProject::KindForPath(const std::string& path) {
    const std::string extension = ToLower(PathToUtf8(Utf8ToPath(path).extension()));
    if (extension == ".mfproject") return "project";
    if (extension == ".mffunction") return "function";
    if (extension == ".mfinstance") return "instance";
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".bmp" || extension == ".tga" || extension == ".hdr" ||
        extension == ".exr" || extension == ".dds" || extension == ".webp") {
        return "texture";
    }
    if (extension == ".obj" || extension == ".fbx" || extension == ".gltf" ||
        extension == ".glb" || extension == ".stl" || extension == ".blend" ||
        extension == ".dae") {
        return "model";
    }
    if (extension == ".json") return "material";
    if (extension == ".txt" || extension == ".md" || extension == ".ini" ||
        extension == ".yaml" || extension == ".yml" || extension == ".toml") {
        return "text";
    }
    if (extension == ".glsl" || extension == ".hlsl" || extension == ".usf" ||
        extension == ".ush") {
        return "shader";
    }
    return "file";
}

namespace {

std::string TruncateUtf8Bytes(const std::string& value, std::size_t maxBytes) {
    if (value.size() <= maxBytes) return value;
    std::size_t end = maxBytes;
    while (end > 0 &&
           (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
        --end;
    }
    return value.substr(0, end);
}

} // namespace

std::string MaterialProject::SanitizeProjectName(const std::string& name) {
    std::string out = Trim(name);
    if (out.empty()) out = "NewProject";
    for (char& ch : out) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) out = "NewProject";
    if (IsWindowsReservedName(out)) out.insert(out.begin(), '_');
    if (out.size() > 96) {
        out = TruncateUtf8Bytes(out, 96);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        if (out.empty()) out = "NewProject";
    }
    return out;
}

bool MaterialProject::IsSafeRelativePath(const std::string& path) {
    if (path.empty()) return false;
    const fs::path relative = Utf8ToPath(NormalizeRelativeSeparators(path));
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    for (const fs::path& component : relative) {
        if (component == ".." || component == ".") return false;
    }
    return !relative.generic_u8string().empty();
}

bool MaterialProject::IsValidFolderName(const std::string& name, std::string* reason) {
    return IsValidFileComponent(name, nullptr, reason);
}

std::optional<MaterialProject> MaterialProject::Create(const fs::path& parentDirectory,
                                                       const std::string& projectName,
                                                       std::string* error) {
    const std::string name = Trim(projectName);
    if (name.empty()) {
        if (error) *error = "Project name cannot be empty.";
        return std::nullopt;
    }
    if (parentDirectory.empty()) {
        if (error) *error = "Project location cannot be empty.";
        return std::nullopt;
    }

    const std::string folderName = SanitizeProjectName(name);
    const fs::path parent = NormalizeAbsolute(parentDirectory);
    const fs::path root = (parent / Utf8ToPath(folderName)).lexically_normal();

    std::error_code ec;
    if (fs::exists(root, ec)) {
        if (!fs::is_directory(root, ec)) {
            if (error) *error = "The project path exists and is not a directory.";
            return std::nullopt;
        }
        if (fs::directory_iterator(root, ec) != fs::directory_iterator()) {
            if (error) *error = "The project folder already exists and is not empty.";
            return std::nullopt;
        }
    }

    fs::create_directories(root, ec);
    if (ec) {
        if (error) *error = "Could not create project folder: " + ec.message();
        return std::nullopt;
    }
    fs::create_directories(root / "materials", ec);
    fs::create_directories(root / "assets" / "textures", ec);
    fs::create_directories(root / "assets" / "models", ec);
    if (ec) {
        if (error) *error = "Could not create project structure: " + ec.message();
        return std::nullopt;
    }

    MaterialProject project;
    project.m_Name = name;
    project.m_RootPath = NormalizeAbsolute(root);
    project.m_ProjectFile = project.m_RootPath / Utf8ToPath(folderName + ".mfproject");
    project.m_MainMaterial = "materials/M_" + folderName + ".json";
    project.m_CreatedAt = NowIso8601();
    project.m_LastOpenedAt = project.m_CreatedAt;
    if (!project.WriteManifest(error)) {
        return std::nullopt;
    }
    return project;
}

std::optional<MaterialProject> MaterialProject::Load(const fs::path& projectFile, std::string* error) {
    const fs::path absolute = NormalizeAbsolute(projectFile);
    std::error_code ec;
    if (!fs::is_regular_file(absolute, ec)) {
        if (error) *error = "Project file does not exist: " + PathToUtf8(absolute);
        return std::nullopt;
    }

    std::string readError;
    std::string contents = ReadTextFile(absolute, &readError);
    if (contents.empty() && !readError.empty()) {
        if (error) *error = readError;
        return std::nullopt;
    }
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }

    json document;
    try {
        document = json::parse(contents);
    } catch (const std::exception& exception) {
        if (error) *error = "Project file is not valid JSON: " + std::string(exception.what());
        return std::nullopt;
    }
    if (!document.is_object()) {
        if (error) *error = "Project file root must be a JSON object.";
        return std::nullopt;
    }
    if (document.value("schema", std::string()) != kProjectSchemaId) {
        if (error) *error = "Not a MaterialForge project file (unsupported schema).";
        return std::nullopt;
    }
    if (document.value("version", 0) != kProjectFormatVersion) {
        if (error) *error = "Unsupported project format version " +
                            std::to_string(document.value("version", 0)) + ".";
        return std::nullopt;
    }

    MaterialProject project;
    project.m_ProjectFile = absolute;
    project.m_RootPath = absolute.parent_path();
    project.m_Name = Trim(document.value("name", std::string()));
    if (project.m_Name.empty()) {
        project.m_Name = PathToUtf8(absolute.stem());
    }
    project.m_CreatedAt = document.value("created", std::string());
    project.m_LastOpenedAt = document.value("lastOpened", std::string());
    project.m_MainMaterial = NormalizeRelativeSeparators(document.value("mainMaterial", std::string()));

    if (document.contains("resources")) {
        if (!document["resources"].is_array()) {
            if (error) *error = "Project 'resources' must be an array.";
            return std::nullopt;
        }
        for (const json& resourceJson : document["resources"]) {
            if (!resourceJson.is_object()) continue;
            ProjectResource resource;
            resource.path = NormalizeRelativeSeparators(
                resourceJson.value("path", std::string()));
            resource.kind = resourceJson.value("kind", std::string("file"));
            resource.required = resourceJson.value("required", true);
            if (resource.path.empty()) continue;
            project.m_Resources.push_back(std::move(resource));
        }
    }

    if (document.contains("folderColors") && document["folderColors"].is_object()) {
        for (auto it = document["folderColors"].begin();
             it != document["folderColors"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string path = NormalizeRelativeSeparators(it.key());
            if (path.empty() || !IsSafeRelativePath(path)) continue;
            project.m_FolderColors[path] = it.value().get<std::string>();
        }
    }

    project.CollectIssues();
    if (error) error->clear();
    return project;
}

bool MaterialProject::Save(std::string* error) {
    if (!IsOpen()) {
        if (error) *error = "No project is open.";
        return false;
    }
    m_LastOpenedAt = NowIso8601();
    return WriteManifest(error);
}

bool MaterialProject::WriteManifest(std::string* error) {
    json document;
    document["schema"] = kProjectSchemaId;
    document["version"] = kProjectFormatVersion;
    document["name"] = m_Name;
    document["created"] = m_CreatedAt;
    document["lastOpened"] = m_LastOpenedAt;
    document["mainMaterial"] = m_MainMaterial;
    document["resources"] = json::array();
    for (const ProjectResource& resource : m_Resources) {
        document["resources"].push_back({
            {"path", resource.path},
            {"kind", resource.kind},
            {"required", resource.required}
        });
    }
    document["folderColors"] = json::object();
    for (const auto& [path, color] : m_FolderColors) {
        document["folderColors"][path] = color;
    }
    return WriteTextAtomically(m_ProjectFile, document.dump(2) + "\n", error);
}

void MaterialProject::CollectIssues() {
    m_Issues.clear();
    auto checkResource = [&](const std::string& path, const char* label, bool required,
                             const std::string& missingReason) {
        if (!required) return;
        if (!IsSafeRelativePath(path)) {
            AddIssue(path.empty() ? label : path,
                     path.empty() ? "No path is configured." : "Invalid relative path.");
            return;
        }
        std::error_code ec;
        const fs::path absolute = ResolveRelative(path);
        if (absolute.empty() || !fs::exists(absolute, ec)) {
            AddIssue(path, missingReason);
        } else if (!fs::is_regular_file(absolute, ec)) {
            AddIssue(path, "Expected a file but found a directory or special file.");
        }
    };

    checkResource(m_MainMaterial, "(main material)", true, "Marked main material is missing.");
    for (const ProjectResource& resource : m_Resources) {
        checkResource(resource.path, "(resource)", resource.required, "Marked project file is missing.");
    }

    std::sort(m_Issues.begin(), m_Issues.end(), [](const ProjectIntegrityIssue& a,
                                                    const ProjectIntegrityIssue& b) {
        return a.path < b.path;
    });
    m_Issues.erase(std::unique(m_Issues.begin(), m_Issues.end(),
                               [](const ProjectIntegrityIssue& a, const ProjectIntegrityIssue& b) {
                                   return a.path == b.path && a.reason == b.reason;
                               }),
                   m_Issues.end());
}

void MaterialProject::AddIssue(std::string path, std::string reason) {
    m_Issues.push_back({std::move(path), std::move(reason)});
}

bool MaterialProject::RefreshIntegrity(std::string* error) {
    if (!IsOpen()) {
        if (error) *error = "No project is open.";
        return false;
    }
    CollectIssues();
    if (error) error->clear();
    return m_Issues.empty();
}

bool MaterialProject::ResolveSafeDirectory(const std::string& relativeDirectory,
                                           fs::path* resolved,
                                           std::string* error) const {
    if (!IsSafeRelativePath(relativeDirectory) && !relativeDirectory.empty()) {
        if (error) *error = "Invalid project-relative directory.";
        return false;
    }
    const fs::path directory = ResolveRelative(relativeDirectory);
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        if (error) *error = "Project directory does not exist: " + relativeDirectory;
        return false;
    }
    if (resolved) *resolved = directory;
    return true;
}

bool MaterialProject::CreateFolder(const std::string& relativeParent, const std::string& folderName,
                                   std::string* error) {
    std::string cleaned;
    std::string reason;
    if (!IsValidFileComponent(folderName, &cleaned, &reason)) {
        if (error) *error = reason;
        return false;
    }
    fs::path parent;
    if (!ResolveSafeDirectory(relativeParent, &parent, error)) {
        return false;
    }
    const fs::path target = parent / Utf8ToPath(cleaned);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        if (error) *error = "A file or folder with that name already exists.";
        return false;
    }
    if (!fs::create_directory(target, ec) || ec) {
        if (error) *error = "Could not create folder: " + ec.message();
        return false;
    }
    if (error) error->clear();
    return true;
}

bool MaterialProject::ImportFile(const fs::path& sourceFile, const std::string& relativeDirectory,
                                 std::string* importedRelativePath, std::string* error) {
    const fs::path source = NormalizeAbsolute(sourceFile);
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        if (error) *error = "Source is not a regular file: " + PathToUtf8(source);
        return false;
    }

    fs::path destinationDirectory;
    if (!ResolveSafeDirectory(relativeDirectory, &destinationDirectory, error)) {
        return false;
    }

    fs::path imported;
    if (Contains(source)) {
        // The file already belongs to the project; mark it rather than making a
        // duplicate copy.
        imported = source;
    } else {
        std::string baseName = PathToUtf8(source.filename());
        if (baseName.empty()) baseName = "imported_file";
        fs::path destination = destinationDirectory / Utf8ToPath(baseName);
        if (fs::exists(destination, ec)) {
            const std::string stem = PathToUtf8(Utf8ToPath(baseName).stem());
            const std::string extension = PathToUtf8(Utf8ToPath(baseName).extension());
            for (int index = 1; index < 10000; ++index) {
                const std::string candidate = stem + "_" + std::to_string(index) + extension;
                destination = destinationDirectory / Utf8ToPath(candidate);
                if (!fs::exists(destination, ec)) break;
            }
        }
        if (fs::exists(destination, ec)) {
            if (error) *error = "Could not choose a unique destination file name.";
            return false;
        }
        fs::copy_file(source, destination, fs::copy_options::none, ec);
        if (ec) {
            if (error) *error = "Could not copy file into project: " + ec.message();
            return false;
        }
        imported = NormalizeAbsolute(destination);
    }

    std::string relative = RelativePathOf(imported);
    if (relative.empty()) {
        if (error) *error = "Imported file is outside the project root.";
        return false;
    }
    if (!AddResource(relative, "auto", true)) {
        if (error) *error = "Could not register the imported file in the project manifest.";
        return false;
    }
    if (!WriteManifest(error)) {
        return false;
    }
    if (importedRelativePath) *importedRelativePath = relative;
    if (error) error->clear();
    return true;
}

bool MaterialProject::AddResource(const std::string& relativePath, const std::string& kind,
                                  bool required) {
    const std::string path = NormalizeRelativeSeparators(relativePath);
    if (!IsSafeRelativePath(path)) {
        return false;
    }
    const std::string resolvedKind = (kind.empty() || kind == "auto") ? KindForPath(path) : kind;
    for (ProjectResource& resource : m_Resources) {
        if (resource.path == path) {
            resource.kind = resolvedKind;
            resource.required = resource.required || required;
            return true;
        }
    }
    m_Resources.push_back({path, resolvedKind, required});
    return true;
}

bool MaterialProject::MoveEntryTo(const std::string& relativeSource,
                                  const std::string& relativeTargetDirectory,
                                  const std::string& newName, std::string* error) {
    if (!IsOpen()) {
        if (error) *error = "No project is open.";
        return false;
    }
    const std::string source = NormalizeRelativeSeparators(relativeSource);
    const std::string targetDirectory = NormalizeRelativeSeparators(relativeTargetDirectory);
    if (source.empty() || !IsSafeRelativePath(source) ||
        (!targetDirectory.empty() && !IsSafeRelativePath(targetDirectory))) {
        if (error) *error = "The source or destination path is invalid.";
        return false;
    }
    std::string name;
    std::string nameReason;
    if (!IsValidFileComponent(newName, &name, &nameReason)) {
        if (error) *error = nameReason;
        return false;
    }

    const fs::path sourceAbsolute = ResolveRelative(source);
    const fs::path targetDirectoryAbsolute =
        targetDirectory.empty() ? m_RootPath : ResolveRelative(targetDirectory);
    if (sourceAbsolute.empty() || targetDirectoryAbsolute.empty()) {
        if (error) *error = "The source or destination is outside the project.";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(sourceAbsolute, ec)) {
        if (error) *error = "The source entry no longer exists.";
        return false;
    }
    if (!fs::is_directory(targetDirectoryAbsolute, ec)) {
        if (error) *error = "The destination folder no longer exists.";
        return false;
    }

    const fs::path destination = targetDirectoryAbsolute / Utf8ToPath(name);
    if (PathsEqual(sourceAbsolute, destination)) {
        if (error) error->clear();
        return true;
    }
    if (fs::exists(destination, ec)) {
        if (error) *error = "A file or folder with that name already exists.";
        return false;
    }
    if (fs::is_directory(sourceAbsolute, ec)) {
        const std::string sourcePrefix = source + "/";
        if (targetDirectory == source || targetDirectory.rfind(sourcePrefix, 0) == 0) {
            if (error) *error = "A folder cannot be moved inside itself.";
            return false;
        }
    }

    fs::rename(sourceAbsolute, destination, ec);
    if (ec) {
        if (error) *error = "Could not move the project entry: " + ec.message();
        return false;
    }

    const std::string destinationRelative =
        targetDirectory.empty() ? name : targetDirectory + "/" + name;
    const auto replacePrefix = [&](std::string& value) {
        if (value == source) {
            value = destinationRelative;
        } else if (value.rfind(source + "/", 0) == 0) {
            value = destinationRelative + value.substr(source.size());
        }
    };
    for (ProjectResource& resource : m_Resources) replacePrefix(resource.path);
    replacePrefix(m_MainMaterial);
    std::map<std::string, std::string> colors;
    for (const auto& [path, color] : m_FolderColors) {
        std::string updated = path;
        replacePrefix(updated);
        colors[updated] = color;
    }
    m_FolderColors = std::move(colors);
    CollectIssues();
    if (error) error->clear();
    return true;
}

bool MaterialProject::MoveEntry(const std::string& relativeSource,
                                const std::string& relativeTargetDirectory,
                                std::string* error) {
    const fs::path source = Utf8ToPath(NormalizeRelativeSeparators(relativeSource));
    if (source.filename().empty()) {
        if (error) *error = "The source path is invalid.";
        return false;
    }
    return MoveEntryTo(relativeSource, relativeTargetDirectory,
                       PathToUtf8(source.filename()), error);
}

bool MaterialProject::DeleteEntry(const std::string& relativePath, bool recursive,
                                  std::string* error) {
    const std::string path = NormalizeRelativeSeparators(relativePath);
    if (path.empty() || !IsSafeRelativePath(path)) {
        if (error) *error = "The project path is invalid.";
        return false;
    }
    const fs::path absolute = ResolveRelative(path);
    if (absolute.empty()) {
        if (error) *error = "The project entry is outside the project root.";
        return false;
    }
    if (absolute == NormalizeAbsolute(m_ProjectFile)) {
        if (error) *error = "The project file cannot be deleted from the Content Browser.";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(absolute, ec)) {
        if (error) *error = "The project entry no longer exists.";
        return false;
    }
    const bool directory = fs::is_directory(absolute, ec);
    const auto clearReadOnly = [](const fs::path& target, bool recursive) {
        std::error_code permissionError;
        if (recursive) {
            std::error_code walkError;
            for (fs::recursive_directory_iterator it(target, walkError), end;
                 !walkError && it != end; it.increment(walkError)) {
                fs::permissions(it->path(), fs::perms::owner_write,
                                fs::perm_options::add, permissionError);
            }
        }
        fs::permissions(target, fs::perms::owner_write, fs::perm_options::add,
                        permissionError);
    };
    if (directory && !recursive && !fs::is_empty(absolute, ec)) {
        if (error) *error = "The folder is not empty. Enable recursive deletion.";
        return false;
    }
    std::error_code removeError;
    for (int attempt = 0; attempt < 5; ++attempt) {
        clearReadOnly(absolute, directory);
        removeError.clear();
        if (directory) {
            fs::remove_all(absolute, removeError);
        } else {
            fs::remove(absolute, removeError);
        }
        if (!removeError) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(60 * (attempt + 1)));
    }
    if (removeError) {
        if (error) {
            *error = "Could not delete the project entry: " +
                     FilesystemErrorText(removeError) + " (" + PathToUtf8(absolute) +
                     ")";
        }
        return false;
    }

    const auto affects = [&](const std::string& candidate) {
        return candidate == path || candidate.rfind(path + "/", 0) == 0;
    };
    m_Resources.erase(
        std::remove_if(m_Resources.begin(), m_Resources.end(),
                       [&](const ProjectResource& resource) {
                           return affects(resource.path);
                       }),
        m_Resources.end());
    for (auto it = m_FolderColors.begin(); it != m_FolderColors.end();) {
        if (affects(it->first)) {
            it = m_FolderColors.erase(it);
        } else {
            ++it;
        }
    }
    if (affects(m_MainMaterial)) {
        m_MainMaterial.clear();
        std::error_code scanError;
        for (fs::recursive_directory_iterator it(m_RootPath, scanError), end;
             !scanError && it != end; it.increment(scanError)) {
            if (!it->is_regular_file(scanError)) continue;
            if (it->path().extension() != ".json") continue;
            m_MainMaterial = RelativePathOf(it->path());
            if (!m_MainMaterial.empty()) break;
        }
    }
    CollectIssues();
    if (!WriteManifest(error)) {
        return false;
    }
    if (error) error->clear();
    return true;
}

bool MaterialProject::RenameEntry(const std::string& relativePath, const std::string& newName,
                                  std::string* error) {
    const std::string source = NormalizeRelativeSeparators(relativePath);
    if (source.empty() || !IsSafeRelativePath(source)) {
        if (error) *error = "The entry path is invalid.";
        return false;
    }
    std::string cleaned;
    std::string reason;
    if (!IsValidFileComponent(newName, &cleaned, &reason)) {
        if (error) *error = reason;
        return false;
    }
    const fs::path sourcePath = Utf8ToPath(source);
    const std::string parent = sourcePath.parent_path().generic_u8string();
    return MoveEntryTo(source, parent == "." ? std::string() : parent, cleaned, error);
}

bool MaterialProject::CopyProjectFile(const std::string& relativeSource,
                               const std::string& relativeTargetDirectory,
                               std::string* importedRelativePath, std::string* error) {
    if (!IsOpen()) {
        if (error) *error = "No project is open.";
        return false;
    }
    const std::string sourceRelative = NormalizeRelativeSeparators(relativeSource);
    const std::string targetRelative = NormalizeRelativeSeparators(relativeTargetDirectory);
    if (!IsSafeRelativePath(sourceRelative) ||
        (!targetRelative.empty() && !IsSafeRelativePath(targetRelative))) {
        if (error) *error = "The source or destination path is invalid.";
        return false;
    }
    const fs::path source = ResolveRelative(sourceRelative);
    const fs::path targetDirectory =
        targetRelative.empty() ? m_RootPath : ResolveRelative(targetRelative);
    std::error_code ec;
    if (source.empty() || !fs::is_regular_file(source, ec)) {
        if (error) *error = "The source file no longer exists.";
        return false;
    }
    if (targetDirectory.empty() || !fs::is_directory(targetDirectory, ec)) {
        if (error) *error = "The destination folder no longer exists.";
        return false;
    }

    const std::string baseName = PathToUtf8(source.filename());
    const std::string stem = PathToUtf8(source.stem());
    const std::string extension = PathToUtf8(source.extension());
    fs::path destination = targetDirectory / Utf8ToPath(baseName);
    for (int index = 1; fs::exists(destination, ec) && index < 10000; ++index) {
        destination = targetDirectory / Utf8ToPath(stem + "_" + std::to_string(index) + extension);
    }
    if (fs::exists(destination, ec)) {
        if (error) *error = "Could not choose a unique destination file name.";
        return false;
    }
    fs::copy_file(source, destination, fs::copy_options::none, ec);
    if (ec) {
        if (error) *error = "Could not copy the file: " + ec.message();
        return false;
    }
    const std::string imported = RelativePathOf(destination);
    if (imported.empty()) {
        if (error) *error = "The copied file is outside the project.";
        return false;
    }
    AddResource(imported, "auto", true);
    if (importedRelativePath) *importedRelativePath = imported;
    if (error) error->clear();
    return true;
}

bool MaterialProject::SetFolderColor(const std::string& relativeFolder,
                                     const std::string& color, std::string* error) {
    const std::string path = NormalizeRelativeSeparators(relativeFolder);
    if (path.empty() || !IsSafeRelativePath(path)) {
        if (error) *error = "The folder path is invalid.";
        return false;
    }
    if (color.empty()) {
        m_FolderColors.erase(path);
    } else {
        m_FolderColors[path] = color;
    }
    if (error) error->clear();
    return true;
}

std::string MaterialProject::FolderColor(const std::string& relativeFolder) const {
    const std::string path = NormalizeRelativeSeparators(relativeFolder);
    const auto it = m_FolderColors.find(path);
    return it == m_FolderColors.end() ? std::string() : it->second;
}

bool MaterialProject::SetMainMaterial(const std::string& relativePath, std::string* error) {
    const std::string path = NormalizeRelativeSeparators(relativePath);
    if (!IsSafeRelativePath(path)) {
        if (error) *error = "Main material path must be relative to the project root.";
        return false;
    }
    m_MainMaterial = path;
    if (error) error->clear();
    return true;
}

std::filesystem::path MaterialProject::ResolveRelative(const std::string& relativePath) const {
    if (!IsOpen() || (!relativePath.empty() && !IsSafeRelativePath(relativePath))) {
        return {};
    }
    if (relativePath.empty()) return m_RootPath;
    const fs::path result = NormalizeAbsolute(m_RootPath / Utf8ToPath(relativePath));
    if (!Contains(result)) return {};
    return result;
}

std::filesystem::path MaterialProject::MainMaterialAbsolutePath() const {
    if (m_MainMaterial.empty()) return {};
    return ResolveRelative(m_MainMaterial);
}

bool MaterialProject::Contains(const std::filesystem::path& absolutePath) const {
    if (!IsOpen() || absolutePath.empty()) return false;
    const fs::path root = NormalizeAbsolute(m_RootPath);
    const fs::path candidate = NormalizeAbsolute(absolutePath);
    std::error_code ec;
    const fs::path relative = fs::relative(candidate, root, ec);
    if (ec || relative.empty()) return false;
    const std::string generic = relative.generic_u8string();
    return generic != ".." && generic.rfind("../", 0) != 0;
}

std::string MaterialProject::RelativePathOf(const std::filesystem::path& absolutePath) const {
    if (!Contains(absolutePath)) return {};
    std::error_code ec;
    const fs::path relative = fs::relative(NormalizeAbsolute(absolutePath), m_RootPath, ec);
    if (ec) return {};
    return NormalizeRelativeSeparators(relative.generic_u8string());
}

std::vector<ProjectEntry> MaterialProject::ListDirectory(const std::string& relativeDirectory,
                                                          std::string* error) const {
    std::vector<ProjectEntry> entries;
    if (!IsOpen()) {
        if (error) *error = "No project is open.";
        return entries;
    }
    fs::path directory;
    if (!ResolveSafeDirectory(relativeDirectory, &directory, error)) {
        return entries;
    }

    std::error_code ec;
    fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (error) *error = "Could not read project directory: " + ec.message();
        return entries;
    }
    for (const fs::directory_entry& item : iterator) {
        const std::string name = PathToUtf8(item.path().filename());
        if (name.empty() || name == "." || name == "..") continue;
        if (!name.empty() && name.front() == '.') continue;
        std::error_code itemError;
        if (item.is_symlink(itemError)) continue;
        if (item.is_directory(itemError)) {
            entries.push_back({name, JoinRelative(relativeDirectory, name), true, 0, "folder"});
        } else if (item.is_regular_file(itemError)) {
            entries.push_back({name, JoinRelative(relativeDirectory, name), false,
                               item.file_size(itemError), KindForPath(name)});
        }
    }

    std::sort(entries.begin(), entries.end(), [](const ProjectEntry& a, const ProjectEntry& b) {
        if (a.directory != b.directory) return a.directory > b.directory;
        return ToLower(a.name) < ToLower(b.name);
    });
    if (error) error->clear();
    return entries;
}

RecentProjects::RecentProjects(std::filesystem::path storePath)
    : m_StorePath(std::move(storePath)) {}

void RecentProjects::SetStorePath(std::filesystem::path storePath) {
    m_StorePath = std::move(storePath);
}

bool RecentProjects::Load(std::string* error) {
    m_Entries.clear();
    if (m_StorePath.empty()) {
        if (error) *error = "Recent projects store path is not configured.";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(m_StorePath, ec)) {
        if (error) error->clear();
        return true;
    }

    std::string readError;
    std::string contents = ReadTextFile(m_StorePath, &readError);
    if (contents.empty() && !readError.empty()) {
        if (error) *error = readError;
        return false;
    }
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }

    try {
        const json document = json::parse(contents);
        if (!document.is_object() || !document.contains("projects") ||
            !document["projects"].is_array()) {
            if (error) *error = "Recent projects file has an unsupported format.";
            return false;
        }
        for (const json& item : document["projects"]) {
            if (!item.is_object()) continue;
            const std::string path = item.value("path", std::string());
            if (path.empty()) continue;
            RecentProjectEntry entry;
            entry.projectFile = NormalizeAbsolute(Utf8ToPath(path));
            entry.name = item.value("name", PathToUtf8(entry.projectFile.stem()));
            entry.lastOpened = item.value("lastOpened", std::string());
            bool duplicate = false;
            for (const RecentProjectEntry& existing : m_Entries) {
                if (PathsEqual(existing.projectFile, entry.projectFile)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) m_Entries.push_back(std::move(entry));
            if (m_Entries.size() >= kMaxRecentProjects) break;
        }
    } catch (const std::exception& exception) {
        if (error) *error = "Could not parse recent projects: " + std::string(exception.what());
        return false;
    }
    if (error) error->clear();
    return true;
}

bool RecentProjects::Save(std::string* error) const {
    if (m_StorePath.empty()) {
        if (error) *error = "Recent projects store path is not configured.";
        return false;
    }
    json document;
    document["schema"] = kRecentProjectsSchema;
    document["version"] = 1;
    document["projects"] = json::array();
    for (const RecentProjectEntry& entry : m_Entries) {
        document["projects"].push_back({
            {"path", PathToUtf8(entry.projectFile)},
            {"name", entry.name},
            {"lastOpened", entry.lastOpened}
        });
    }
    return WriteTextAtomically(m_StorePath, document.dump(2) + "\n", error);
}

void RecentProjects::Touch(const std::filesystem::path& projectFile, const std::string& name) {
    const fs::path absolute = NormalizeAbsolute(projectFile);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [&](const RecentProjectEntry& entry) {
                                       return PathsEqual(entry.projectFile, absolute);
                                   }),
                    m_Entries.end());
    RecentProjectEntry entry;
    entry.projectFile = absolute;
    entry.name = Trim(name).empty() ? PathToUtf8(absolute.stem()) : Trim(name);
    entry.lastOpened = NowIso8601();
    m_Entries.insert(m_Entries.begin(), std::move(entry));
    if (m_Entries.size() > kMaxRecentProjects) {
        m_Entries.resize(kMaxRecentProjects);
    }
}

void RecentProjects::Remove(const std::filesystem::path& projectFile) {
    const fs::path absolute = NormalizeAbsolute(projectFile);
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [&](const RecentProjectEntry& entry) {
                                       return PathsEqual(entry.projectFile, absolute);
                                   }),
                    m_Entries.end());
}

void RecentProjects::Clear() {
    m_Entries.clear();
}

} // namespace mf
