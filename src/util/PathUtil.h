#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mf::PathUtil {

std::filesystem::path ExecutablePath();
std::filesystem::path ExecutableDirectory();
std::filesystem::path CurrentDirectory();
std::filesystem::path UserHomeDirectory();
std::filesystem::path UserDocumentsDirectory();
std::filesystem::path UserConfigDirectory();
std::filesystem::path DefaultProjectsDirectory();
std::filesystem::path FindProjectRoot();

// Returns the first existing candidate for a (possibly relative) resource
// path. Search order: working directory, executable directory, its parents,
// and the discovered project root.
std::filesystem::path ResolveResource(const std::string& path);

std::string ToUtf8(const std::filesystem::path& path);
std::filesystem::path FromUtf8(const std::string& path);

// If `path` is inside `base`, returns a generic relative path, otherwise the
// absolute path. Useful for keeping texture references portable in JSON.
std::string MakePortablePath(const std::filesystem::path& path,
                             const std::filesystem::path& base);

bool Exists(const std::filesystem::path& path);

} // namespace mf::PathUtil
