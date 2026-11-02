#include "libretranslate_client.h"
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

LibreTranslateClient::LibreTranslateClient() 
    : tokens_(60), last_refill_(0), rate_limited_(false), rate_limit_reset_(0) {
    // Initialize curl globally (once)
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
    
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

LibreTranslateClient::~LibreTranslateClient() {
}

void LibreTranslateClient::Initialize(const Config& config) {
    config_ = config;
    tokens_ = config.rate_limit_per_min;
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    rate_limited_ = false;
    rate_limit_reset_ = 0;
    cached_languages_.clear();
}

std::string LibreTranslateClient::BuildTranslateUrl() const {
    return config_.base_url;
}

std::string LibreTranslateClient::BuildLanguagesUrl() const {
    // Extract base URL without /translate
    size_t pos = config_.base_url.find("/translate");
    if (pos != std::string::npos) {
        return config_.base_url.substr(0, pos) + "/languages";
    }
    return config_.base_url + "/languages";
}

std::string LibreTranslateClient::BuildDetectUrl() const {
    size_t pos = config_.base_url.find("/translate");
    if (pos != std::string::npos) {
        return config_.base_url.substr(0, pos) + "/detect";
    }
    return config_.base_url + "/detect";
}

std::string LibreTranslateClient::BuildTranslateJson(const std::string& text, 
                                                     const std::string& source_lang, 
                                                     const std::string& target_lang) const {
    nlohmann::json j;
    j["q"] = text;
    j["source"] = source_lang;
    j["target"] = target_lang;
    j["format"] = "text";
    if (!config_.api_key.empty()) {
        j["api_key"] = config_.api_key;
    }
    return j.dump();
}

std::string LibreTranslateClient::BuildBatchJson(const std::vector<std::string>& texts,
                                                 const std::string& source_lang,
                                                 const std::string& target_lang) const {
    nlohmann::json j;
    j["q"] = texts;
    j["source"] = source_lang;
    j["target"] = target_lang;
    j["format"] = "text";
    if (!config_.api_key.empty()) {
        j["api_key"] = config_.api_key;
    }
    return j.dump();
}

void LibreTranslateClient::RefillTokens() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    int64_t elapsed = now - last_refill_.load();
    if (elapsed >= 60) {  // Refill per minute
        int new_tokens = std::min(config_.rate_limit_per_min, 
                                  tokens_.load() + static_cast<int>(elapsed / 60 * config_.rate_limit_per_min));
        tokens_.store(new_tokens);
        last_refill_.store(now);
    }
}

bool LibreTranslateClient::ConsumeToken() {
    RefillTokens();
    
    int current = tokens_.load();
    while (current > 0) {
        if (tokens_.compare_exchange_weak(current, current - 1)) {
            return true;
        }
    }
    return false;
}

void LibreTranslateClient::HandleRateLimit(long http_code, const std::string& response) {
    if (http_code == 429) {
        rate_limited_.store(true);
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        rate_limit_reset_.store(now + 60); // Default 60 seconds
        tokens_.store(0);
        
        // Try to parse retry-after from response
        try {
            nlohmann::json j = nlohmann::json::parse(response);
            if (j.contains("error") && j["error"].is_string()) {
                std::string error = j["error"].get<std::string>();
                // Try to extract number from error message
                size_t pos = error.find_first_of("0123456789");
                if (pos != std::string::npos) {
                    int retry = std::stoi(error.substr(pos));
                    if (retry > 0 && retry < 3600) {
                        rate_limit_reset_.store(now + retry);
                    }
                }
            }
        } catch (...) {
            // Ignore parse errors
        }
        
        ErrorLog("LibreTranslate rate limited, retry after ~60 seconds");
    }
}

std::string LibreTranslateClient::PerformRequest(const std::string& url, const std::string& post_data) {
    // Wait for token
    while (!ConsumeToken()) {
        if (rate_limited_.load()) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t reset = rate_limit_reset_.load();
            if (now < reset) {
                std::this_thread::sleep_for(std::chrono::seconds(reset - now + 1));
                rate_limited_.store(false);
                tokens_.store(config_.rate_limit_per_min);
                last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        ErrorLog("Failed to initialize curl");
        return "";
    }
    
    std::string response;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_data.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config_.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    // Headers
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
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
        HandleRateLimit(http_code, response);
        return "";
    }
    
    if (http_code != 200) {
        ErrorLog("LibreTranslate API error: HTTP {} for URL: {}", http_code, url);
        ErrorLog("Response: {}", response);
        return "";
    }
    
    return response;
}

std::optional<std::string> LibreTranslateClient::ParseTranslationResponse(const std::string& response) {
    try {
        nlohmann::json j = nlohmann::json::parse(response);
        
        if (j.contains("translatedText") && j["translatedText"].is_string()) {
            return j["translatedText"].get<std::string>();
        }
        
        // Check for error
        if (j.contains("error")) {
            ErrorLog("LibreTranslate error: {}", j["error"].get<std::string>());
        }
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse LibreTranslate response: {}", e.what());
    }
    return std::nullopt;
}

std::vector<std::optional<std::string>> LibreTranslateClient::ParseBatchResponse(const std::string& response, size_t expected_count) {
    std::vector<std::optional<std::string>> results;
    results.reserve(expected_count);
    
    try {
        nlohmann::json j = nlohmann::json::parse(response);
        
        if (j.is_array()) {
            for (const auto& item : j) {
                if (item.contains("translatedText") && item["translatedText"].is_string()) {
                    results.push_back(item["translatedText"].get<std::string>());
                } else {
                    results.push_back(std::nullopt);
                }
            }
        } else if (j.contains("translatedText") && j["translatedText"].is_string()) {
            // Single result wrapped
            results.push_back(j["translatedText"].get<std::string>());
        } else if (j.contains("error")) {
            ErrorLog("LibreTranslate batch error: {}", j["error"].get<std::string>());
        }
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse LibreTranslate batch response: {}", e.what());
    }
    
    // Pad with nullopt if fewer results
    while (results.size() < expected_count) {
        results.push_back(std::nullopt);
    }
    
    return results;
}

std::optional<std::string> LibreTranslateClient::Translate(const std::string& text, 
                                                           const std::string& source_lang, 
                                                           const std::string& target_lang) {
    if (text.empty()) return std::nullopt;
    if (source_lang == target_lang) return text;
    
    std::string url = BuildTranslateUrl();
    std::string json = BuildTranslateJson(text, source_lang, target_lang);
    std::string response = PerformRequest(url, json);
    
    if (response.empty()) return std::nullopt;
    
    return ParseTranslationResponse(response);
}

std::vector<std::optional<std::string>> LibreTranslateClient::TranslateBatch(
    const std::vector<std::string>& texts,
    const std::string& source_lang,
    const std::string& target_lang) {
    
    std::vector<std::optional<std::string>> results;
    if (texts.empty()) return results;
    
    // Filter empty texts
    std::vector<std::string> valid_texts;
    std::vector<size_t> valid_indices;
    for (size_t i = 0; i < texts.size(); ++i) {
        if (!texts[i].empty()) {
            valid_texts.push_back(texts[i]);
            valid_indices.push_back(i);
        } else {
            results.push_back(std::nullopt);
        }
    }
    
    if (valid_texts.empty()) return results;
    
    // LibreTranslate batch limit is typically 1000 chars per request
    // Split into chunks if needed
    const size_t max_chars_per_request = 5000;
    std::vector<std::string> chunk;
    size_t chunk_chars = 0;
    
    for (const auto& text : valid_texts) {
        if (chunk_chars + text.size() > max_chars_per_request && !chunk.empty()) {
            // Process current chunk
            std::string url = BuildTranslateUrl();
            std::string json = BuildBatchJson(chunk, source_lang, target_lang);
            std::string response = PerformRequest(url, json);
            
            if (!response.empty()) {
                auto chunk_results = ParseBatchResponse(response, chunk.size());
                results.insert(results.end(), chunk_results.begin(), chunk_results.end());
            } else {
                // Fill with nullopt for failed chunk
                results.insert(results.end(), chunk.size(), std::nullopt);
            }
            
            chunk.clear();
            chunk_chars = 0;
        }
        
        chunk.push_back(text);
        chunk_chars += text.size();
    }
    
    // Process remaining chunk
    if (!chunk.empty()) {
        std::string url = BuildTranslateUrl();
        std::string json = BuildBatchJson(chunk, source_lang, target_lang);
        std::string response = PerformRequest(url, json);
        
        if (!response.empty()) {
            auto chunk_results = ParseBatchResponse(response, chunk.size());
            results.insert(results.end(), chunk_results.begin(), chunk_results.end());
        } else {
            results.insert(results.end(), chunk.size(), std::nullopt);
        }
    }
    
    // Reorder results to match original indices
    std::vector<std::optional<std::string>> final_results(texts.size());
    size_t result_idx = 0;
    for (size_t i = 0; i < texts.size(); ++i) {
        if (texts[i].empty()) {
            final_results[i] = std::nullopt;
        } else if (result_idx < results.size()) {
            final_results[i] = results[result_idx++];
        } else {
            final_results[i] = std::nullopt;
        }
    }
    
    return final_results;
}

std::optional<CardTranslation> LibreTranslateClient::TranslateCard(const CardTranslation& source_card,
                                                                   const std::string& target_locale) {
    auto* target_lang_info = FindLanguage(target_locale);
    if (!target_lang_info || target_lang_info->libretranslate_code.empty()) {
        ErrorLog("Target locale {} not supported by LibreTranslate", target_locale);
        return std::nullopt;
    }
    
    std::string target_lang = target_lang_info->libretranslate_code;
    std::string source_lang = "en"; // Always translate from English
    
    CardTranslation result = source_card;
    result.locale = target_locale;
    result.source = "libretranslate";
    result.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Translate name
    if (!source_card.name.empty()) {
        if (auto translated = Translate(source_card.name, source_lang, target_lang)) {
            result.name = *translated;
        }
    }
    
    // Translate desc
    if (!source_card.desc.empty()) {
        if (auto translated = Translate(source_card.desc, source_lang, target_lang)) {
            result.desc = *translated;
        }
    }
    
    // Translate strs (str1-str16)
    std::vector<std::string> strs_to_translate;
    std::vector<int> str_indices;
    for (int i = 0; i < 16; ++i) {
        if (!source_card.strs[i].empty()) {
            strs_to_translate.push_back(source_card.strs[i]);
            str_indices.push_back(i);
        }
    }
    
    if (!strs_to_translate.empty()) {
        auto translated_strs = TranslateBatch(strs_to_translate, source_lang, target_lang);
        for (size_t i = 0; i < str_indices.size() && i < translated_strs.size(); ++i) {
            if (translated_strs[i]) {
                result.strs[str_indices[i]] = *translated_strs[i];
            }
        }
    }
    
    return result;
}

std::vector<LibreTranslateClient::Language> LibreTranslateClient::GetSupportedLanguages() {
    if (!cached_languages_.empty()) {
        return cached_languages_;
    }
    
    std::string url = BuildLanguagesUrl();
    std::string response = PerformRequest(url, "");
    
    if (response.empty()) {
        // Return known languages as fallback
        cached_languages_ = {
            {"en", "English"}, {"pt", "Portuguese"}, {"fr", "French"},
            {"de", "German"}, {"it", "Italian"}, {"zh", "Chinese"},
            {"es", "Spanish"}, {"ar", "Arabic"}, {"bn", "Bengali"},
            {"ru", "Russian"}, {"ja", "Japanese"}, {"ko", "Korean"}
        };
        return cached_languages_;
    }
    
    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.is_array()) {
            for (const auto& item : j) {
                if (item.contains("code") && item.contains("name")) {
                    cached_languages_.push_back({
                        item["code"].get<std::string>(),
                        item["name"].get<std::string>()
                    });
                }
            }
        }
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse languages response: {}", e.what());
    }
    
    return cached_languages_;
}

bool LibreTranslateClient::IsLanguageSupported(const std::string& lang_code) const {
    auto langs = const_cast<LibreTranslateClient*>(this)->GetSupportedLanguages();
    return std::any_of(langs.begin(), langs.end(), 
                       [&lang_code](const Language& l) { return l.code == lang_code; });
}

std::optional<std::string> LibreTranslateClient::DetectLanguage(const std::string& text) {
    if (text.empty()) return std::nullopt;
    
    std::string url = BuildDetectUrl();
    nlohmann::json j;
    j["q"] = text;
    if (!config_.api_key.empty()) {
        j["api_key"] = config_.api_key;
    }
    
    std::string response = PerformRequest(url, j.dump());
    if (response.empty()) return std::nullopt;
    
    try {
        nlohmann::json result = nlohmann::json::parse(response);
        if (result.is_array() && !result.empty()) {
            auto first = result[0];
            if (first.contains("language") && first.contains("confidence")) {
                double confidence = first["confidence"].get<double>();
                if (confidence > 0.5) {
                    return first["language"].get<std::string>();
                }
            }
        }
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse detect response: {}", e.what());
    }
    
    return std::nullopt;
}

bool LibreTranslateClient::IsRateLimited() const {
    return rate_limited_.load();
}

int LibreTranslateClient::GetRemainingCalls() const {
    return tokens_.load();
}

std::chrono::seconds LibreTranslateClient::GetRetryAfter() const {
    if (!rate_limited_.load()) return std::chrono::seconds(0);
    
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t reset = rate_limit_reset_.load();
    
    if (now >= reset) return std::chrono::seconds(0);
    return std::chrono::seconds(reset - now);
}

} // namespace ygo