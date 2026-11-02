#include "ygoprodeck_client.h"
#include "utils.h"
#include "logging.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <sstream>
#include <algorithm>

namespace ygo {

// Static write callback for libcurl
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

// Static header callback for rate limit headers
static size_t HeaderCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

YGOPRODeckClient::YGOPRODeckClient() 
    : tokens_(20), last_refill_(0), rate_limited_(false), rate_limit_reset_(0) {
    // Initialize curl globally (once)
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
    
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

YGOPRODeckClient::~YGOPRODeckClient() {
}

void YGOPRODeckClient::Initialize(const Config& config) {
    config_ = config;
    tokens_ = config.rate_limit_per_sec;
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    rate_limited_ = false;
    rate_limit_reset_ = 0;
}

std::string YGOPRODeckClient::BuildCardUrl(uint32_t card_id, const std::string& language_code) const {
    std::ostringstream oss;
    oss << config_.base_url << "?id=" << card_id;
    if (!language_code.empty()) {
        oss << "&language=" << language_code;
    }
    return oss.str();
}

std::string YGOPRODeckClient::BuildBatchUrl(const std::vector<uint32_t>& card_ids, const std::string& language_code) const {
    std::ostringstream oss;
    oss << config_.base_url << "?id=";
    for (size_t i = 0; i < card_ids.size(); ++i) {
        if (i > 0) oss << ",";
        oss << card_ids[i];
    }
    if (!language_code.empty()) {
        oss << "&language=" << language_code;
    }
    return oss.str();
}

void YGOPRODeckClient::RefillTokens() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    int64_t elapsed = now - last_refill_.load();
    if (elapsed >= 1) {
        int new_tokens = std::min(config_.rate_limit_per_sec, tokens_.load() + static_cast<int>(elapsed * config_.rate_limit_per_sec));
        tokens_.store(new_tokens);
        last_refill_.store(now);
    }
}

bool YGOPRODeckClient::ConsumeToken() {
    RefillTokens();
    
    int current = tokens_.load();
    while (current > 0) {
        if (tokens_.compare_exchange_weak(current, current - 1)) {
            return true;
        }
    }
    return false;
}

void YGOPRODeckClient::HandleRateLimit(const std::string& response_headers) {
    // Check for rate limit headers
    // YGOPRODeck returns 429 with Retry-After header when rate limited
    size_t pos = response_headers.find("Retry-After:");
    if (pos != std::string::npos) {
        pos += 12; // Skip "Retry-After:"
        while (pos < response_headers.size() && (response_headers[pos] == ' ' || response_headers[pos] == '\t')) pos++;
        
        int retry_after = 0;
        try {
            retry_after = std::stoi(response_headers.substr(pos));
        } catch (...) {
            retry_after = 60; // Default 60 seconds
        }
        
        rate_limited_.store(true);
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        rate_limit_reset_.store(now + retry_after);
        tokens_.store(0);
        
        ErrorLog("YGOPRODeck rate limited, retry after {} seconds", retry_after);
    }
}

std::string YGOPRODeckClient::PerformRequest(const std::string& url) {
    // Wait for token
    while (!ConsumeToken()) {
        if (rate_limited_.load()) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t reset = rate_limit_reset_.load();
            if (now < reset) {
                std::this_thread::sleep_for(std::chrono::seconds(reset - now + 1));
                rate_limited_.store(false);
                tokens_.store(config_.rate_limit_per_sec);
                last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        ErrorLog("Failed to initialize curl");
        return "";
    }
    
    std::string response;
    std::string headers;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config_.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    // Add Accept header for JSON
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    
    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        ErrorLog("Curl error: {}", curl_easy_strerror(res));
        return "";
    }
    
    if (http_code == 429) {
        HandleRateLimit(headers);
        return ""; // Rate limited
    }
    
    if (http_code != 200) {
        ErrorLog("YGOPRODeck API error: HTTP {} for URL: {}", http_code, url);
        return "";
    }
    
    return response;
}

std::optional<YGOPRODeckClient::CardData> YGOPRODeckClient::ParseCardResponse(const nlohmann::json& json) {
    if (!json.contains("data") || !json["data"].is_array() || json["data"].empty()) {
        return std::nullopt;
    }
    
    const auto& card = json["data"][0];
    
    CardData data;
    data.id = card.value("id", 0);
    data.name = card.value("name", "");
    data.desc = card.value("desc", "");
    data.type = card.value("type", "");
    data.frameType = card.value("frameType", "");
    data.atk = card.value("atk", 0);
    data.def = card.value("def", 0);
    data.level = card.value("level", 0);
    data.race = card.value("race", "");
    data.attribute = card.value("attribute", "");
    
    if (card.contains("card_images") && card["card_images"].is_array()) {
        for (const auto& img : card["card_images"]) {
            if (img.contains("image_url")) {
                data.card_images.push_back(img["image_url"].get<std::string>());
            }
        }
    }
    
    if (card.contains("card_prices") && card["card_prices"].is_array()) {
        for (const auto& price : card["card_prices"]) {
            std::ostringstream oss;
            oss << "cardmarket: " << price.value("cardmarket_price", "N/A") 
                << ", tcgplayer: " << price.value("tcgplayer_price", "N/A")
                << ", ebay: " << price.value("ebay_price", "N/A")
                << ", amazon: " << price.value("amazon_price", "N/A")
                << ", coolstuffinc: " << price.value("coolstuffinc_price", "N/A");
            data.card_prices.push_back(oss.str());
        }
    }
    
    return data;
}

std::optional<YGOPRODeckClient::CardData> YGOPRODeckClient::FetchCard(uint32_t card_id, const std::string& language_code) {
    std::string url = BuildCardUrl(card_id, language_code);
    std::string response = PerformRequest(url);
    
    if (response.empty()) {
        return std::nullopt;
    }
    
    try {
        nlohmann::json json = nlohmann::json::parse(response);
        return ParseCardResponse(json);
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse YGOPRODeck response: {}", e.what());
        return std::nullopt;
    }
}

std::vector<YGOPRODeckClient::CardData> YGOPRODeckClient::FetchCards(const std::vector<uint32_t>& card_ids, const std::string& language_code) {
    std::vector<CardData> results;
    if (card_ids.empty()) return results;
    
    // Process in batches of 20 (API limit)
    const size_t batch_size = 20;
    for (size_t i = 0; i < card_ids.size(); i += batch_size) {
        size_t end = std::min(i + batch_size, card_ids.size());
        std::vector<uint32_t> batch(card_ids.begin() + i, card_ids.begin() + end);
        
        std::string url = BuildBatchUrl(batch, language_code);
        std::string response = PerformRequest(url);
        
        if (response.empty()) continue;
        
        try {
            nlohmann::json json = nlohmann::json::parse(response);
            if (json.contains("data") && json["data"].is_array()) {
                for (const auto& card : json["data"]) {
                    if (auto parsed = ParseCardResponse(nlohmann::json{{"data", {card}}})) {
                        results.push_back(*parsed);
                    }
                }
            }
        } catch (const std::exception& e) {
            ErrorLog("Failed to parse YGOPRODeck batch response: {}", e.what());
        }
    }
    
    return results;
}

bool YGOPRODeckClient::IsLanguageSupported(const std::string& language_code) const {
    static const std::vector<std::string> supported = {"en", "fr", "de", "it", "pt"};
    return std::find(supported.begin(), supported.end(), language_code) != supported.end();
}

std::vector<std::string> YGOPRODeckClient::GetSupportedLanguages() {
    return {"en", "fr", "de", "it", "pt"};
}

CardTranslation YGOPRODeckClient::ToCardTranslation(const CardData& data, const std::string& locale) {
    CardTranslation t;
    t.card_id = data.id;
    t.locale = locale;
    t.source = "ygoprodeck";
    t.name = data.name;
    t.desc = data.desc;
    t.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // strs remain empty for now (YGOPRODeck doesn't provide str1-str16 separately)
    // They would need to be parsed from desc if needed
    for (auto& s : t.strs) s.clear();
    
    return t;
}

bool YGOPRODeckClient::IsRateLimited() const {
    return rate_limited_.load();
}

int YGOPRODeckClient::GetRemainingCalls() const {
    return tokens_.load();
}

std::chrono::seconds YGOPRODeckClient::GetRetryAfter() const {
    if (!rate_limited_.load()) return std::chrono::seconds(0);
    
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t reset = rate_limit_reset_.load();
    
    if (now >= reset) return std::chrono::seconds(0);
    return std::chrono::seconds(reset - now);
}

} // namespace ygo