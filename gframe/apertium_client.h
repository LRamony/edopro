#ifndef APERTIUM_CLIENT_H
#define APERTIUM_CLIENT_H

#include "card_translation.h"
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <atomic>

namespace ygo {

class ApertiumClient {
public:
    struct Config {
        std::string base_url = "https://apertium.org/apy";
        int timeout_seconds = 30;
        int rate_limit_per_min = 60;
        std::string user_agent = "EDOPro/41.0.0 (Card Translation)";
    };

    ApertiumClient();
    ~ApertiumClient();

    void Initialize(const Config& config = Config());
    std::optional<CardTranslation> TranslateCard(const CardTranslation& source_card,
                                                 const std::string& target_locale);

private:
    struct LanguagePair {
        std::string source;
        std::string target;
    };

    Config config_;
    std::mutex pairs_mutex_;
    std::vector<LanguagePair> language_pairs_;
    bool language_pairs_loaded_ = false;
    std::atomic<int> tokens_;
    std::atomic<int64_t> last_refill_;

    bool ConsumeToken();
    void RefillTokens();
    std::string PerformRequest(const std::string& url);
    bool LoadLanguagePairs();
    std::optional<std::vector<std::string>> FindRoute(const std::string& source,
                                                      const std::string& target);
    std::optional<std::string> TranslateText(const std::string& text,
                                             const std::string& target);
    std::optional<std::string> TranslatePair(const std::string& text,
                                            const std::string& source,
                                            const std::string& target);
};

} // namespace ygo

#endif // APERTIUM_CLIENT_H
