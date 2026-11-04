#include "apertium_client.h"
#include "logging.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <queue>
#include <thread>

namespace ygo {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    response->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string UrlEncode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        ErrorLog("Failed to initialize URL encoder for Apertium");
        return {};
    }
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!encoded) {
        ErrorLog("Failed to encode Apertium request parameter");
        curl_easy_cleanup(curl);
        return {};
    }
    std::string result = encoded ? encoded : "";
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

} // namespace

ApertiumClient::ApertiumClient()
    : tokens_(60), last_refill_(0) {
    static const CURLcode curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_init_result != CURLE_OK)
        ErrorLog("Failed to initialize curl for Apertium");
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ApertiumClient::~ApertiumClient() = default;

void ApertiumClient::Initialize(const Config& config) {
    config_ = config;
    tokens_ = config.rate_limit_per_min;
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ApertiumClient::RefillTokens() {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t elapsed = now - last_refill_.load();
    if (elapsed >= 60) {
        const int refill = static_cast<int>(elapsed / 60) * config_.rate_limit_per_min;
        tokens_.store(std::min(config_.rate_limit_per_min, tokens_.load() + refill));
        last_refill_.store(now);
    }
}

bool ApertiumClient::ConsumeToken() {
    RefillTokens();
    int current = tokens_.load();
    while (current > 0) {
        if (tokens_.compare_exchange_weak(current, current - 1))
            return true;
    }
    return false;
}

std::string ApertiumClient::PerformRequest(const std::string& url) {
    while (!ConsumeToken())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CURL* curl = curl_easy_init();
    if (!curl) {
        ErrorLog("Failed to initialize curl request for Apertium");
        return {};
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config_.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode result = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        ErrorLog("Apertium request failed: {}", curl_easy_strerror(result));
        return {};
    }
    if (http_code != 200) {
        ErrorLog("Apertium API error: HTTP {} for URL: {}", http_code, url);
        return {};
    }
    return response;
}

bool ApertiumClient::LoadLanguagePairs() {
    std::lock_guard<std::mutex> lock(pairs_mutex_);
    if (language_pairs_loaded_)
        return !language_pairs_.empty();

    const std::string response = PerformRequest(config_.base_url + "/listPairs");
    if (response.empty())
        return false;

    try {
        const auto json = nlohmann::json::parse(response);
        if (!json.contains("responseData") || !json["responseData"].is_array()) {
            ErrorLog("Apertium language-pair response has an unexpected format");
            return false;
        }

        for (const auto& pair : json["responseData"]) {
            if (pair.contains("sourceLanguage") && pair["sourceLanguage"].is_string() &&
                pair.contains("targetLanguage") && pair["targetLanguage"].is_string()) {
                language_pairs_.push_back({
                    pair["sourceLanguage"].get<std::string>(),
                    pair["targetLanguage"].get<std::string>()
                });
            }
        }
        language_pairs_loaded_ = true;
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse Apertium language-pair response: {}", e.what());
        return false;
    }

    if (language_pairs_.empty())
        ErrorLog("Apertium returned no supported language pairs");
    return !language_pairs_.empty();
}

std::optional<std::vector<std::string>> ApertiumClient::FindRoute(const std::string& source,
                                                                  const std::string& target) {
    if (!LoadLanguagePairs())
        return std::nullopt;
    if (source == target)
        return std::vector<std::string>{source};

    std::queue<std::vector<std::string>> routes;
    routes.push({source});
    std::vector<std::string> visited{source};
    constexpr size_t max_hops = 2;

    while (!routes.empty()) {
        auto route = std::move(routes.front());
        routes.pop();
        if (route.size() > max_hops)
            continue;

        const auto& current = route.back();
        std::vector<const LanguagePair*> outgoing_pairs;
        for (const auto& pair : language_pairs_) {
            if (pair.source == current)
                outgoing_pairs.push_back(&pair);
        }
        if (current == "eng") {
            static const std::vector<std::string> preferred_pivots = {"spa", "cat", "glg", "epo"};
            const auto priority = [](const LanguagePair* pair) {
                const auto it = std::find(preferred_pivots.begin(), preferred_pivots.end(), pair->target);
                return std::distance(preferred_pivots.begin(), it);
            };
            std::stable_sort(outgoing_pairs.begin(), outgoing_pairs.end(),
                [&priority](const LanguagePair* lhs, const LanguagePair* rhs) {
                    return priority(lhs) < priority(rhs);
                });
        }
        for (const auto* pair : outgoing_pairs) {
            if (std::find(visited.begin(), visited.end(), pair->target) != visited.end())
                continue;

            auto next_route = route;
            next_route.push_back(pair->target);
            if (pair->target == target) {
                if (next_route.back() != target)
                    continue;
                return next_route;
            }
            visited.push_back(pair->target);
            routes.push(std::move(next_route));
        }
    }

    return std::nullopt;
}

std::optional<std::string> ApertiumClient::TranslatePair(const std::string& text,
                                                         const std::string& source,
                                                         const std::string& target) {
    const std::string url = config_.base_url + "/translate?langpair=" +
        UrlEncode(source + "|" + target) + "&markUnknown=no&q=" + UrlEncode(text);
    const std::string response = PerformRequest(url);
    if (response.empty())
        return std::nullopt;

    try {
        const auto json = nlohmann::json::parse(response);
        if (json.contains("responseData") && json["responseData"].is_object() &&
            json["responseData"].contains("translatedText") &&
            json["responseData"]["translatedText"].is_string()) {
            return json["responseData"]["translatedText"].get<std::string>();
        }
        if (json.contains("responseDetails") && json["responseDetails"].is_string())
            ErrorLog("Apertium translation error: {}", json["responseDetails"].get<std::string>());
        else
            ErrorLog("Apertium translation response has an unexpected format");
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse Apertium translation response: {}", e.what());
    }
    return std::nullopt;
}

std::optional<std::string> ApertiumClient::TranslateText(const std::string& text,
                                                          const std::string& target) {
    if (text.empty())
        return std::nullopt;

    const auto route = FindRoute("eng", target);
    if (!route) {
        ErrorLog("Apertium has no translation route from eng to {}", target);
        return std::nullopt;
    }
    if (route->empty() || route->back() != target) {
        ErrorLog("Apertium route does not end in the requested language {}", target);
        return std::nullopt;
    }

    std::string translated = text;
    for (size_t i = 1; i < route->size(); ++i) {
        auto step = TranslatePair(translated, (*route)[i - 1], (*route)[i]);
        if (!step)
            return std::nullopt;
        translated = std::move(*step);
    }
    return translated;
}

std::optional<CardTranslation> ApertiumClient::TranslateCard(const CardTranslation& source_card,
                                                             const std::string& target_locale) {
    const std::string target = GetApertiumCode(target_locale);
    if (target.empty()) {
        ErrorLog("Target locale {} is not mapped to an Apertium language", target_locale);
        return std::nullopt;
    }

    if (!FindRoute("eng", target)) {
        ErrorLog("Apertium has no translation route from eng to {} ({})", target_locale, target);
        return std::nullopt;
    }

    CardTranslation result = source_card;
    result.locale = target_locale;
    result.source = "apertium:" + target;
    result.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (auto name = TranslateText(source_card.name, target))
        result.name = std::move(*name);
    if (auto desc = TranslateText(source_card.desc, target))
        result.desc = std::move(*desc);
    for (size_t i = 0; i < source_card.strs.size(); ++i) {
        if (auto text = TranslateText(source_card.strs[i], target))
            result.strs[i] = std::move(*text);
    }

    return result;
}

} // namespace ygo
