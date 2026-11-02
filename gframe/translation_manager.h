#ifndef TRANSLATION_MANAGER_H
#define TRANSLATION_MANAGER_H

#include "card_translation.h"
#include "translation_cache.h"
#include "ygoprodeck_client.h"
#include "libretranslate_client.h"
#include "data_manager.h"
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>
#include <functional>
#include <sqlite3.h>
#include <utility>

namespace ygo {

class TranslationManager {
public:
    struct Config {
        bool enabled = true;
        std::string locale = "pt-BR";
        std::string locale_db_path = "./config/languages/";
        
        // YGOPRODeck settings
        bool ygoprodeck_enabled = true;
        int ygoprodeck_rate_limit = 20;
        
        // LibreTranslate settings
        bool libretranslate_enabled = true;
        std::string libretranslate_url = "https://libretranslate.de/translate";
        std::string libretranslate_api_key = "";
        int libretranslate_rate_limit = 60;
        
        // Cache settings
        int cache_max_age_days = 30;
        size_t cache_max_entries = 50000;
        
        // Behavior
        bool translate_on_demand = true;      // Translate when card is viewed
        bool preload_popular = false;         // Preload popular cards
        bool fallback_to_english = true;      // Show English if translation fails
    };
    
    struct Stats {
        size_t cache_hits = 0;
        size_t cache_misses = 0;
        size_t ygoprodeck_calls = 0;
        size_t libretranslate_calls = 0;
        size_t locale_db_writes = 0;
        size_t translations_done = 0;
        size_t errors = 0;
    };
    
    TranslationManager();
    ~TranslationManager();
    
    // Initialize the translation system
    bool Initialize(const Config& config);
    
    // Main interface: ensure card is translated for current locale
    // Returns true if translation is available (from cache, DB, or fresh translation)
    bool EnsureCardTranslated(uint32_t card_code, const std::string& locale = "");
    
    // Get translation for a card (from cache/DB, no API call)
    std::optional<CardTranslation> GetTranslation(uint32_t card_code, const std::string& locale = "");
    
    // Force translate a card (bypass cache, always call API)
    bool ForceTranslateCard(uint32_t card_code, const std::string& locale = "");
    
    // Translate multiple cards
    size_t TranslateCards(const std::vector<uint32_t>& card_codes, const std::string& locale = "");
    
    // Set current locale (called when user changes language in settings)
    void SetLocale(const std::string& locale);
    std::string GetLocale() const;
    
    // Get statistics
    Stats GetStats() const;
    TranslationCache::Stats GetCacheStats() const;
    
    // Cleanup old cache entries
    size_t CleanupCache();
    
    // Check if translation is enabled
    bool IsEnabled() const;
    void SetEnabled(bool enabled);
    bool IsTranslateOnDemand() const;
    void SetTranslateOnDemand(bool enabled);
    
    // Save locale strings (strings.conf) for current locale
    bool SaveLocaleStrings(const std::string& locale = "");
    
    // Called when card is viewed in UI (Game::ShowCardInfo)
    void OnCardViewed(uint32_t card_code, const std::string& locale = "");
    std::vector<uint32_t> TakeCompletedTranslations(const std::string& locale);
    
private:
    Config config_;
    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> translate_on_demand_{true};
    std::atomic<std::size_t> cache_hits_{0};
    std::atomic<std::size_t> cache_misses_{0};
    std::atomic<std::size_t> ygoprodeck_calls_{0};
    std::atomic<std::size_t> libretranslate_calls_{0};
    std::atomic<std::size_t> locale_db_writes_{0};
    std::atomic<std::size_t> translations_done_{0};
    std::atomic<std::size_t> errors_{0};
    
    // Components
    std::unique_ptr<TranslationCache> cache_;
    std::unique_ptr<YGOPRODeckClient> ygoprodeck_;
    std::unique_ptr<LibreTranslateClient> libretranslate_;
    
    // Current locale
    std::string current_locale_;
    
    // Background worker for async translations
    std::thread worker_thread_;
    std::atomic<bool> worker_running_{false};
    std::mutex queue_mutex_;
    std::vector<std::pair<uint32_t, std::string>> translation_queue_;
    std::vector<std::pair<uint32_t, std::string>> pending_translations_;
    std::vector<std::pair<uint32_t, std::string>> completed_translations_;
    
    // Internal methods
    std::optional<CardTranslation> TryGetFromCache(uint32_t card_code, const std::string& locale);
    std::optional<CardTranslation> TryGetFromLocaleDB(uint32_t card_code, const std::string& locale);
    std::optional<CardTranslation> TryFetchFromYGOPRODeck(uint32_t card_code, const std::string& locale);
    std::optional<CardTranslation> TryTranslateWithLibreTranslate(uint32_t card_code, const std::string& locale);
    bool SaveToLocaleDB(const CardTranslation& translation);
    bool EnsureLocaleDBExists(const std::string& locale);
    
    // Worker thread
    void WorkerLoop();
    void QueueTranslation(uint32_t card_code, const std::string& locale);
    
    // Helpers
    std::string GetLocaleDBPath(const std::string& locale) const;
    std::string GetLocaleStringsPath(const std::string& locale) const;
};

extern std::unique_ptr<TranslationManager> gTranslationManager;

} // namespace ygo

#endif // TRANSLATION_MANAGER_H