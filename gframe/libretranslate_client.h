#ifndef LIBRETRANSLATE_CLIENT_H
#define LIBRETRANSLATE_CLIENT_H

#include "card_translation.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <mutex>
#include <atomic>

namespace ygo {

class LibreTranslateClient {
public:
    struct Config {
        std::string base_url = "https://libretranslate.de/translate";
        int timeout_seconds = 30;
        std::string api_key = "";  // Optional API key for higher limits
        int rate_limit_per_min = 60;
        std::string user_agent = "EDOPro/41.0.0 (Translation System)";
    };
    
    struct Language {
        std::string code;
        std::string name;
    };
    
    struct TranslationResult {
        std::string translated_text;
        std::string detected_language;
    };
    
    LibreTranslateClient();
    ~LibreTranslateClient();
    
    // Initialize with config
    void Initialize(const Config& config = Config());
    
    // Translate single text
    std::optional<std::string> Translate(const std::string& text, 
                                         const std::string& source_lang, 
                                         const std::string& target_lang);
    
    // Translate multiple texts (batch)
    std::vector<std::optional<std::string>> TranslateBatch(
        const std::vector<std::string>& texts,
        const std::string& source_lang,
        const std::string& target_lang);
    
    // Translate card (name + desc)
    std::optional<CardTranslation> TranslateCard(const CardTranslation& source_card,
                                                 const std::string& target_locale);
    
    // Get supported languages
    std::vector<Language> GetSupportedLanguages();
    
    // Check if language pair is supported
    bool IsLanguageSupported(const std::string& lang_code) const;
    
    // Auto-detect source language
    std::optional<std::string> DetectLanguage(const std::string& text);
    
    // Rate limiting status
    bool IsRateLimited() const;
    int GetRemainingCalls() const;
    std::chrono::seconds GetRetryAfter() const;
    
private:
    Config config_;
    mutable std::mutex mutex_;
    
    // Rate limiting
    std::atomic<int> tokens_;
    std::atomic<int64_t> last_refill_;
    std::atomic<bool> rate_limited_;
    std::atomic<int64_t> rate_limit_reset_;
    std::vector<Language> cached_languages_;
    
    // HTTP request
    std::string PerformRequest(const std::string& url, const std::string& post_data);
    std::optional<std::string> ParseTranslationResponse(const std::string& response);
    std::vector<std::optional<std::string>> ParseBatchResponse(const std::string& response, size_t expected_count);
    
    // Rate limiting
    void RefillTokens();
    bool ConsumeToken();
    void HandleRateLimit(long http_code, const std::string& response);
    
    // URL building
    std::string BuildTranslateUrl() const;
    std::string BuildLanguagesUrl() const;
    std::string BuildDetectUrl() const;
    
    // JSON building
    std::string BuildTranslateJson(const std::string& text, 
                                   const std::string& source_lang, 
                                   const std::string& target_lang) const;
    std::string BuildBatchJson(const std::vector<std::string>& texts,
                               const std::string& source_lang,
                               const std::string& target_lang) const;
};

} // namespace ygo

#endif // LIBRETRANSLATE_CLIENT_H