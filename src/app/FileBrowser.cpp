#include "app/FileBrowser.h"

#include "app/Localization.h"
#include "app/UiWidgets.h"
#include "util/PathUtil.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>

namespace fs = std::filesystem;

namespace mf {
namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string LocalizedFileError(const std::string& error) {
    const char* exact = Tr(error.c_str());
    if (exact != error.c_str()) {
        return exact;
    }
    static const char* const prefixes[] = {
        "Directory does not exist: ",
        "Cannot read directory: ",
        "File does not exist: ",
        "Could not open file: ",
    };
    for (const char* prefix : prefixes) {
        const std::string key(prefix);
        if (error.rfind(key, 0) == 0) {
            return std::string(Tr(prefix)) + error.substr(key.size());
        }
    }
    return error;
}

std::string NormalizeExtension(std::string extension) {
    extension = ToLower(std::move(extension));
    if (!extension.empty() && extension.front() != '.') {
        extension.insert(extension.begin(), '.');
    }
    return extension;
}

std::string HumanSize(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    }
    return buffer;
}

bool DrawAccentButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.55f, 0.52f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.68f, 0.63f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.48f, 0.46f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

} // namespace

void FileBrowser::Open(std::string title, Mode mode, const fs::path& startDirectory,
                       std::string defaultFileName, std::vector<std::string> extensions) {
    m_Title = std::move(title);
    m_Mode = mode;
    m_Extensions.clear();
    for (std::string& extension : extensions) {
        m_Extensions.push_back(NormalizeExtension(std::move(extension)));
    }
    m_FileName = std::move(defaultFileName);
    m_Error.clear();
    m_Result.reset();
    m_Active = true;
    m_PendingOpen = true;
    m_JustOpened = true;

    fs::path directory = startDirectory;
    std::error_code ec;
    if (directory.empty()) {
        directory = PathUtil::CurrentDirectory();
    }
    while (!directory.empty() && !fs::is_directory(directory, ec)) {
        const fs::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) break;
        directory = parent;
    }
    if (!fs::is_directory(directory, ec)) {
        directory = PathUtil::CurrentDirectory();
    }
    m_Directory = fs::absolute(directory, ec);
    if (ec) m_Directory = directory;
    NavigateTo(m_Directory);
}

void FileBrowser::NavigateTo(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    m_Directory = ec ? path.lexically_normal() : absolute.lexically_normal();
    m_DirectoryText = PathUtil::ToUtf8(m_Directory);
    RefreshDirectory();
}

void FileBrowser::RefreshDirectory() {
    m_Entries.clear();
    m_Error.clear();
    std::error_code ec;
    if (!fs::is_directory(m_Directory, ec)) {
        m_Error = "Directory does not exist: " + PathUtil::ToUtf8(m_Directory);
        return;
    }

    fs::directory_iterator iterator(m_Directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        m_Error = "Cannot read directory: " + ec.message();
        return;
    }

    for (const fs::directory_entry& entry : iterator) {
        const std::string name = PathUtil::ToUtf8(entry.path().filename());
        if (name.empty() || name == "." || name == "..") {
            continue;
        }
        if (!name.empty() && name.front() == '.') {
            continue;
        }
        std::error_code entryError;
        if (entry.is_directory(entryError)) {
            m_Entries.push_back({name, true, 0});
        } else if (m_Mode != Mode::SelectFolder && entry.is_regular_file(entryError)) {
            if (!m_Extensions.empty()) {
                const std::string extension = ToLower(PathUtil::ToUtf8(entry.path().extension()));
                const bool matches = std::find(m_Extensions.begin(), m_Extensions.end(), extension) !=
                                     m_Extensions.end();
                if (!matches) {
                    continue;
                }
            }
            m_Entries.push_back({name, false, entry.file_size(entryError)});
        }
    }

    std::sort(m_Entries.begin(), m_Entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory > b.directory;
        return ToLower(a.name) < ToLower(b.name);
    });
}

bool FileBrowser::Accept() {
    if (m_Mode == Mode::SelectFolder) {
        std::error_code ec;
        if (!fs::is_directory(m_Directory, ec)) {
            m_Error = "Directory does not exist.";
            return false;
        }
        m_Result = PathUtil::ToUtf8(m_Directory);
        ImGui::CloseCurrentPopup();
        return true;
    }

    std::string fileName = m_FileName;
    const auto first = fileName.find_first_not_of(" \t\r\n");
    const auto last = fileName.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        m_Error = "Please enter a file name.";
        return false;
    }
    fileName = fileName.substr(first, last - first + 1);

    fs::path path = PathUtil::FromUtf8(fileName);
    if (path.is_relative()) {
        path = m_Directory / path;
    }

    if (m_Mode == Mode::Save) {
        if (!path.has_extension() && !m_Extensions.empty()) {
            path.replace_extension(PathUtil::FromUtf8(m_Extensions.front()));
        }
    }

    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    const fs::path normalized = (ec ? path : absolute).lexically_normal();

    if (m_Mode == Mode::Open) {
        if (!fs::is_regular_file(normalized, ec)) {
            m_Error = "File does not exist: " + PathUtil::ToUtf8(normalized);
            return false;
        }
    } else {
        if (fs::is_directory(normalized, ec)) {
            m_Error = "The target path is a directory.";
            return false;
        }
        if (!normalized.has_filename()) {
            m_Error = "Please enter a file name.";
            return false;
        }
    }

    m_Result = PathUtil::ToUtf8(normalized);
    ImGui::CloseCurrentPopup();
    return true;
}

void FileBrowser::Cancel() {
    m_Error.clear();
    m_Result.reset();
    m_Active = false;
    if (ImGui::IsPopupOpen(m_Title.c_str())) {
        ImGui::CloseCurrentPopup();
    }
}

std::optional<std::string> FileBrowser::Draw() {
    if (!m_Active) {
        return std::nullopt;
    }

    if (m_PendingOpen) {
        ImGui::OpenPopup(m_Title.c_str());
        m_PendingOpen = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Ui::S(820.0f), Ui::S(580.0f)), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(m_Title.c_str(), &open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (m_JustOpened) {
            m_JustOpened = false;
            m_FocusFileName = true;
        }

        ImGui::TextDisabled("%s", m_Mode == Mode::SelectFolder
                                     ? Tr("Browse to a folder, then press Select Folder.")
                                     : Tr("Browse to a folder, then choose or type a file name."));
        ImGui::Spacing();

        if (ImGui::Button(Tr("Up"))) {
            const fs::path parent = m_Directory.parent_path();
            if (!parent.empty() && parent != m_Directory) {
                NavigateTo(parent);
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-74.0f);
        if (ImGui::InputText("##directory", &m_DirectoryText, ImGuiInputTextFlags_EnterReturnsTrue)) {
            NavigateTo(PathUtil::FromUtf8(m_DirectoryText));
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Go"), ImVec2(Ui::S(72.0f), 0.0f))) {
            NavigateTo(PathUtil::FromUtf8(m_DirectoryText));
        }

        const float footerHeight = m_Mode == Mode::SelectFolder ? 94.0f : 112.0f;
        ImGui::BeginChild("##entries", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
        if (!m_Error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.42f, 1.0f));
            ImGui::TextWrapped("%s", LocalizedFileError(m_Error).c_str());
            ImGui::PopStyleColor();
        }
        for (const Entry& entry : m_Entries) {
            ImGui::PushID(entry.name.c_str());
            if (entry.directory) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.82f, 0.92f, 1.0f));
                const std::string label = std::string(Tr("[DIR]")) + "  " + entry.name;
                if (ImGui::Selectable(label.c_str())) {
                    NavigateTo(m_Directory / PathUtil::FromUtf8(entry.name));
                }
                ImGui::PopStyleColor();
            } else {
                const bool selected = (entry.name == m_FileName);
                const std::string label = "       " + entry.name;
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None,
                                      ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
                    m_FileName = entry.name;
                }
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                const ImVec2 rowMax = ImGui::GetItemRectMax();
                const std::string sizeText = HumanSize(entry.size);
                const ImVec2 size = ImGui::CalcTextSize(sizeText.c_str());
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rowMax.x - size.x - 8.0f, (rowMin.y + rowMax.y - size.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), sizeText.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_FileName = entry.name;
                    if (m_Mode == Mode::Open) {
                        Accept();
                    }
                }
            }
            ImGui::PopID();
        }
        if (m_Entries.empty() && m_Error.empty()) {
            ImGui::TextDisabled("%s", m_Mode == Mode::SelectFolder
                                          ? Tr("No folders in this folder.")
                                          : Tr("No matching files in this folder."));
        }
        ImGui::EndChild();

        ImGui::Separator();
        bool acceptWithEnter = false;
        if (m_Mode == Mode::SelectFolder) {
            const std::string folderName = PathUtil::ToUtf8(m_Directory.filename());
            ImGui::TextUnformatted(Tr("Folder"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.56f, 0.82f, 0.86f, 1.0f), "%s",
                               folderName.empty() ? PathUtil::ToUtf8(m_Directory).c_str()
                                                  : folderName.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", PathUtil::ToUtf8(m_Directory).c_str());
            }
            if (!m_Error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.42f, 1.0f));
                ImGui::TextWrapped("%s", m_Error.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("%s", Tr("Only folders are shown in this mode."));
            }
        } else {
            ImGui::TextUnformatted(Tr("File name"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (m_FocusFileName) {
                ImGui::SetKeyboardFocusHere();
                m_FocusFileName = false;
            }
            acceptWithEnter = ImGui::InputText(
                "##filename", &m_FileName, ImGuiInputTextFlags_EnterReturnsTrue);

            if (!m_Error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.42f, 1.0f));
                ImGui::TextWrapped("%s", m_Error.c_str());
                ImGui::PopStyleColor();
            } else if (m_Mode == Mode::Save) {
                ImGui::TextDisabled("%s", Tr("Existing files will be overwritten."));
            } else {
                std::error_code ec;
                const fs::path candidate = m_Directory / PathUtil::FromUtf8(m_FileName);
                if (!m_FileName.empty() && !fs::is_regular_file(candidate, ec)) {
                    ImGui::TextDisabled("%s", Tr("Select a file or type an absolute path."));
                }
            }
        }

        const float buttonWidth = m_Mode == Mode::SelectFolder ? Ui::S(128.0f) : Ui::S(108.0f);
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonWidth * 2.0f -
                             ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button(Tr("Cancel"), ImVec2(buttonWidth, Ui::S(32.0f)))) {
            Cancel();
        }
        ImGui::SameLine();
        const char* acceptLabel = m_Mode == Mode::Open ? Tr("Open")
                                : m_Mode == Mode::Save ? Tr("Save")
                                                       : Tr("Select Folder");
        if (DrawAccentButton(acceptLabel, ImVec2(buttonWidth, Ui::S(32.0f))) ||
            acceptWithEnter) {
            Accept();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            Cancel();
        }

        ImGui::EndPopup();
        if (!open) {
            m_Active = false;
        }
    } else {
        m_Active = false;
    }

    if (m_Result) {
        const std::string result = *m_Result;
        m_Result.reset();
        m_Active = false;
        return result;
    }
    return std::nullopt;
}

} // namespace mf
