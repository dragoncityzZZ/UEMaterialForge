#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mf {

class FileBrowser {
public:
    enum class Mode {
        Open,
        Save,
        SelectFolder
    };

    void Open(std::string title, Mode mode, const std::filesystem::path& startDirectory,
              std::string defaultFileName, std::vector<std::string> extensions);
    std::optional<std::string> Draw();
    bool IsActive() const { return m_Active; }
    void Cancel();

private:
    struct Entry {
        std::string name;
        bool directory = false;
        std::uintmax_t size = 0;
    };

    void RefreshDirectory();
    void NavigateTo(const std::filesystem::path& path);
    bool Accept();

    bool m_PendingOpen = false;
    bool m_JustOpened = false;
    bool m_Active = false;
    std::string m_Title;
    Mode m_Mode = Mode::Open;
    std::filesystem::path m_Directory;
    std::string m_DirectoryText;
    std::string m_FileName;
    std::vector<std::string> m_Extensions;
    std::vector<Entry> m_Entries;
    std::string m_Error;
    std::optional<std::string> m_Result;
    bool m_FocusFileName = false;
};

} // namespace mf
