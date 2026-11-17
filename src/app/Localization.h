#pragma once

namespace mf {

enum class Language {
    English,
    Chinese
};

Language DetectSystemLanguage();

class Localization {
public:
    static Localization& Get();

    void SetLanguage(Language language);
    Language CurrentLanguage() const { return m_Language; }
    bool IsChinese() const { return m_Language == Language::Chinese; }

    const char* Tr(const char* english) const;
    const char* LanguageCode() const;
    static const char* DisplayName(Language language);

private:
    Language m_Language = Language::English;
};

inline const char* Tr(const char* english) {
    return Localization::Get().Tr(english);
}

inline bool IsChineseUi() {
    return Localization::Get().IsChinese();
}

} // namespace mf
