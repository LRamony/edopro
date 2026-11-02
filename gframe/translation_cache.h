#ifndef TRANSLATION_CACHE_H
#define TRANSLATION_CACHE_H

#include "card_translation.h"
#include <sqlite3.h>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ygo {

class TranslationCache {
public:
    TranslationCache();
    ~TranslationCache();
    
    // Initialize cache database
    bool Initialize(const std::string& cache_path = "./config/translation_cache.db");
    
    // Get cached translation
    std::optional<CardTranslation> Get(uint32_t card_id, const std::string& locale);
    
    // Save translation to cache
    bool Put(const CardTranslation& translation);
    
    // Save multiple translations (batch)
    bool PutBatch(const std::vector<CardTranslation>& translations);
    
    // Check if translation exists in cache
    bool Exists(uint32_t card_id, const std::string& locale);
    
    // Remove old entries (older than max_age_days)
    size_t Cleanup(int max_age_days = 30);
    
    // Get cache statistics
    struct Stats {
        size_t total_entries = 0;
        size_t total_size_bytes = 0;
        int ygoprodeck_calls_today = 0;
        int libretranslate_chars_today = 0;
    };
    Stats GetStats() const;
    
    // Increment API usage counters
    void IncrementYGOPRODeckCalls(int count = 1);
    void IncrementLibreTranslateChars(int chars);
    
    // Get today's API usage
    int GetYGOPRODeckCallsToday() const;
    int GetLibreTranslateCharsToday() const;
    
private:
    std::unique_ptr<sqlite3, void(*)(sqlite3*)> db_;
    std::string cache_path_;
    mutable std::mutex mutex_;
    
    bool CreateTables();
    int64_t GetCurrentTimestamp() const;
    std::string GetTodayDate() const;
};

} // namespace ygo

#endif // TRANSLATION_CACHE_H