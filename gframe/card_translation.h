#ifndef CARD_TRANSLATION_H
#define CARD_TRANSLATION_H

#include <string>
#include <cstdint>
#include <array>

namespace ygo {

struct CardTranslation {
    uint32_t card_id = 0;
    std::string locale;
    std::string source;  // "ygoprodeck", "libretranslate", "manual"
    
    std::string name;
    std::string desc;
    std::array<std::string, 16> strs;  // str1-str16
    
    int64_t updated_at = 0;  // unix timestamp
    
    bool HasContent() const {
        return !name.empty() || !desc.empty();
    }
    
    void ClearStrings() {
        name.clear();
        desc.clear();
        for (auto& s : strs) s.clear();
    }
};

// Supported languages mapping
struct LanguageInfo {
    std::string locale_code;      // EDOPro locale (e.g., "pt-BR")
    std::string ygoprodeck_code;  // YGOPRODeck API code (e.g., "pt") - empty if not supported
    std::string libretranslate_code;  // LibreTranslate code (e.g., "pt")
    std::string display_name;     // UI display name
};

// Returns all 10 supported languages
inline const std::array<LanguageInfo, 10>& GetSupportedLanguages() {
    static const std::array<LanguageInfo, 10> languages = {{
        {"en", "en", "en", "English"},
        {"pt-BR", "pt", "pt", "Português Brasil"},
        {"fr", "fr", "fr", "Français"},
        {"de", "de", "de", "Deutsch"},
        {"it", "it", "it", "Italiano"},
        {"zh-CN", "", "zh", "中文 (简体)"},
        {"es", "", "es", "Español"},
        {"ar", "", "ar", "العربية"},
        {"bn", "", "bn", "বাংলা"},
        {"ru", "", "ru", "Русский"},
        // Note: Urdu "ur" not in LibreTranslate yet, will fallback
    }};
    return languages;
}

// Find language by locale code
inline const LanguageInfo* FindLanguage(const std::string& locale) {
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.locale_code == locale || lang.display_name == locale) return &lang;
    }
    if (locale == "Português") return &GetSupportedLanguages()[1];
    return nullptr;
}

// Check if locale is supported by YGOPRODeck
inline bool IsYGOPRODeckSupported(const std::string& locale) {
    auto* lang = FindLanguage(locale);
    return lang && !lang->ygoprodeck_code.empty();
}

// Check if locale is supported by LibreTranslate
inline bool IsLibreTranslateSupported(const std::string& locale) {
    auto* lang = FindLanguage(locale);
    return lang && !lang->libretranslate_code.empty();
}

// Get YGOPRODeck language code for locale
inline std::string GetYGOPRODeckCode(const std::string& locale) {
    auto* lang = FindLanguage(locale);
    return lang ? lang->ygoprodeck_code : "";
}

// Get LibreTranslate language code for locale
inline std::string GetLibreTranslateCode(const std::string& locale) {
    auto* lang = FindLanguage(locale);
    return lang ? lang->libretranslate_code : "";
}

} // namespace ygo

#endif // CARD_TRANSLATION_H