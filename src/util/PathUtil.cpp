#include "util/PathUtil.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mf::PathUtil {
namespace {

fs::path Ascend(fs::path path) {
    if (path.has_parent_path() && path.parent_path() != path) {
        return path.parent_path();
    }
    return path;
}

} // namespace

fs::path ExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return fs::path(buffer);
    }
#elif defined(__linux__)
    std::vector<char> buffer(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer.resize(static_cast<std::size_t>(length));
        return fs::path(std::string(buffer.begin(), buffer.end()));
    }
#endif
    return CurrentDirectory();
}

fs::path ExecutableDirectory() {
    const fs::path executable = ExecutablePath();
    return executable.has_parent_path() ? executable.parent_path() : CurrentDirectory();
}

fs::path CurrentDirectory() {
    std::error_code ec;
    const fs::path path = fs::current_path(ec);
    return ec ? fs::path(".") : path;
}

fs::path UserHomeDirectory() {
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"); profile && profile[0] != '\0') {
        return fs::u8path(profile);
    }
    if (const char* drive = std::getenv("HOMEDRIVE"); drive) {
        if (const char* path = std::getenv("HOMEPATH"); path) {
            return fs::u8path(std::string(drive) + path);
        }
    }
#else
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return fs::u8path(home);
    }
#endif
    return CurrentDirectory();
}

fs::path UserDocumentsDirectory() {
    const fs::path home = UserHomeDirectory();
    const fs::path documents = home / "Documents";
    std::error_code ec;
    if (fs::is_directory(documents, ec)) {
        return documents;
    }
    return home;
}

fs::path UserConfigDirectory() {
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData && appData[0] != '\0') {
        return fs::u8path(appData) / "MaterialForge";
    }
    return UserHomeDirectory() / "AppData" / "Roaming" / "MaterialForge";
#elif defined(__APPLE__)
    return UserHomeDirectory() / "Library" / "Application Support" / "MaterialForge";
#else
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME"); configHome && configHome[0] != '\0') {
        return fs::u8path(configHome) / "MaterialForge";
    }
    return UserHomeDirectory() / ".config" / "MaterialForge";
#endif
}

fs::path DefaultProjectsDirectory() {
    return UserDocumentsDirectory() / "MaterialForge Projects";
}

bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

fs::path FindProjectRoot() {
    auto looksLikeRoot = [](const fs::path& path) {
        std::error_code ec;
        return fs::exists(path / "CMakeLists.txt", ec) && fs::exists(path / "assets", ec);
    };

    fs::path cursor = CurrentDirectory();
    for (int i = 0; i < 8; ++i) {
        if (looksLikeRoot(cursor)) return cursor;
        const fs::path next = Ascend(cursor);
        if (next == cursor) break;
        cursor = next;
    }

    cursor = ExecutableDirectory();
    for (int i = 0; i < 8; ++i) {
        if (looksLikeRoot(cursor)) return cursor;
        const fs::path next = Ascend(cursor);
        if (next == cursor) break;
        cursor = next;
    }
    return {};
}

fs::path ResolveResource(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const fs::path input = FromUtf8(path);
    std::error_code ec;
    if (input.is_absolute()) {
        return fs::exists(input, ec) ? input : input;
    }

    std::vector<fs::path> roots;
    roots.push_back(CurrentDirectory());
    const fs::path exeDir = ExecutableDirectory();
    roots.push_back(exeDir);
    roots.push_back(Ascend(exeDir));
    roots.push_back(Ascend(Ascend(exeDir)));
    const fs::path projectRoot = FindProjectRoot();
    if (!projectRoot.empty()) {
        roots.push_back(projectRoot);
    }

    for (const fs::path& root : roots) {
        const fs::path candidate = root / input;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return roots.empty() ? input : roots.front() / input;
}

std::string ToUtf8(const fs::path& path) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
#else
    return path.u8string();
#endif
}

fs::path FromUtf8(const std::string& path) {
    return fs::u8path(path);
}

std::string MakePortablePath(const fs::path& path, const fs::path& base) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, base, ec);
    if (!ec && !relative.empty() &&
        relative.generic_string().find("..") == std::string::npos) {
        return relative.generic_u8string();
    }
    return path.generic_u8string();
}

} // namespace mf::PathUtil
