#ifndef YUGIPEDIA_CLIENT_H
#define YUGIPEDIA_CLIENT_H

#include "card_translation.h"
#include <atomic>
#include <optional>
#include <string>

namespace ygo {

class YugipediaClient {
public:
    struct Config {
        std::string base_url = "https://yugipedia.com/api.php";
        int timeout_seconds = 20;
        int rate_limit_per_min = 60;
        std::string user_agent = "EDOPro/41.0.0 (Card Translation)";
    };

    YugipediaClient();
    void Initialize(const Config& config = Config());
    std::optional<std::string> FetchCardImageUrl(const std::string& card_name);
    std::optional<CardTranslation> FetchTranslation(uint32_t card_id,
                                                    const std::string& locale,
                                                    const std::string& card_name);

private:
    Config config_;
    std::atomic<int> tokens_{60};
    std::atomic<int64_t> last_refill_{0};

    void RefillTokens();
    bool ConsumeToken();
    std::string PerformRequest(const std::string& url);
};

} // namespace ygo

#endif // YUGIPEDIA_CLIENT_H
