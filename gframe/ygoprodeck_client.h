#ifndef YGOPRODECK_CLIENT_H
#define YGOPRODECK_CLIENT_H

#include "card_translation.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <chrono>
#include <mutex>
#include <queue>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>

namespace ygo {

class YGOPRODeckClient {
public:
    struct Config {
        std::string base_url = "https://db.ygoprodeck.com/api/v7/cardinfo.php";
        int rate_limit_per_sec = 20;
        int timeout_seconds = 10;
        std::string user_agent = "EDOPro/41.0.0 (Translation System)";
    };
    
    struct CardData {
        uint32_t id = 0;
        std::string name;
        std::string desc;
        std::string type;
        std::string frameType;
        int atk = 0;
        int def = 0;
        int level = 0;
        std::string race;
        std::string attribute;
        std::vector<std::string> card_images;
        std::vector<std::string> card_prices;
    };
    
    YGOPRODeckClient();
    ~YGOPRODeckClient();
    
    // Initialize with config
    void Initialize(const Config& config = Config());
    
    // Fetch card by ID (synchronous, with rate limiting)
    std::optional<CardData> FetchCard(uint32_t card_id, const std::string& language_code = "");
    
    // Fetch multiple cards by IDs (batch)
    std::vector<CardData> FetchCards(const std::vector<uint32_t>& card_ids, const std::string& language_code = "");
    
    // Check if language is supported
    bool IsLanguageSupported(const std::string& language_code) const;
    
    // Get supported language codes
    static std::vector<std::string> GetSupportedLanguages();
    
    // Convert to CardTranslation
    static CardTranslation ToCardTranslation(const CardData& data, const std::string& locale);
    
    // Rate limiting status
    bool IsRateLimited() const;
    int GetRemainingCalls() const;
    std::chrono::seconds GetRetryAfter() const;
    
private:
    Config config_;
    mutable std::mutex mutex_;
    
    // Rate limiting (token bucket)
    std::atomic<int> tokens_;
    std::atomic<int64_t> last_refill_;
    std::atomic<bool> rate_limited_;
    std::atomic<int64_t> rate_limit_reset_;
    
    // HTTP request
    std::string PerformRequest(const std::string& url);
    std::optional<CardData> ParseCardResponse(const nlohmann::json& json);
    
    // Rate limiting
    void RefillTokens();
    bool ConsumeToken();
    void HandleRateLimit(const std::string& response_headers);
    
    // URL building
    std::string BuildCardUrl(uint32_t card_id, const std::string& language_code) const;
    std::string BuildBatchUrl(const std::vector<uint32_t>& card_ids, const std::string& language_code) const;
};

} // namespace ygo

#endif // YGOPRODECK_CLIENT_H