#include "yugipedia_client.h"
#include "logging.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <map>
#include <regex>
#include <thread>
#include <vector>

namespace ygo {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    response->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string UrlEncode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        ErrorLog("Failed to initialize URL encoder for Yugipedia");
        return {};
    }

    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!encoded) {
        ErrorLog("Failed to encode Yugipedia request parameter");
        curl_easy_cleanup(curl);
        return {};
    }

    std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

std::map<std::string, std::string> ParseCardFields(const std::string& wikitext) {
    std::map<std::string, std::string> fields;
    std::string current_field;
    size_t start = 0;

    while (start < wikitext.size()) {
        const auto end = wikitext.find('\n', start);
        const auto line_end = end == std::string::npos ? wikitext.size() : end;
        std::string line = Trim(wikitext.substr(start, line_end - start));
        if (!line.empty() && line.front() == '|') {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                current_field = Trim(line.substr(1, equals - 1));
                fields[current_field] = Trim(line.substr(equals + 1));
            } else {
                current_field.clear();
            }
        } else if (!line.empty() && line != "}}" && !current_field.empty()) {
            fields[current_field] += "\n" + line;
        }

        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return fields;
}

std::string ReplaceRegex(const std::string& text, const std::regex& pattern,
                         const std::string& replacement) {
    return std::regex_replace(text, pattern, replacement);
}

std::string StripWikitext(std::string text) {
    text = ReplaceRegex(text, std::regex(R"(<!--[\s\S]*?-->)"), "");
    text = ReplaceRegex(text, std::regex(R"(<ref\b[^>]*>[\s\S]*?</ref>)", std::regex::icase), "");
    text = ReplaceRegex(text, std::regex(R"(<br\s*/?>)", std::regex::icase), "\n");
    text = ReplaceRegex(text, std::regex(R"(<[^>]+>)"), "");

    const std::regex piped_link_pattern(R"(\[\[[^\]|]+\|([^\]]*)\]\])");
    const std::regex simple_link_pattern(R"(\[\[([^\]|]+)\]\])");
    for (int i = 0; i < 8; ++i) {
        const auto previous = text;
        text = std::regex_replace(text, piped_link_pattern, "$1");
        text = std::regex_replace(text, simple_link_pattern, "$1");
        if (text == previous)
            break;
    }

    text = ReplaceRegex(text, std::regex(R"(\[https?://\S+\s+([^\]]+)\])"), "$1");
    text = ReplaceRegex(text, std::regex(R"(\{\{[^{}]*\}\})"), "");
    text = ReplaceRegex(text, std::regex(R"(''{2,5})"), "");
    text = ReplaceRegex(text, std::regex(R"(&nbsp;)", std::regex::icase), " ");
    text = ReplaceRegex(text, std::regex(R"(&amp;)", std::regex::icase), "&");
    text = ReplaceRegex(text, std::regex(R"(&lt;)", std::regex::icase), "<");
    text = ReplaceRegex(text, std::regex(R"(&gt;)", std::regex::icase), ">");
    text = ReplaceRegex(text, std::regex(R"(&quot;)", std::regex::icase), "\"");
    text = ReplaceRegex(text, std::regex(R"(&#39;|&apos;)", std::regex::icase), "'");
    text = ReplaceRegex(text, std::regex(R"([ \t]+\n)"), "\n");
    return Trim(std::move(text));
}

} // namespace

YugipediaClient::YugipediaClient() {
    static const CURLcode curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_init_result != CURLE_OK)
        ErrorLog("Failed to initialize curl for Yugipedia");
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void YugipediaClient::Initialize(const Config& config) {
    config_ = config;
    tokens_ = config.rate_limit_per_min;
    last_refill_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void YugipediaClient::RefillTokens() {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t elapsed = now - last_refill_.load();
    if (elapsed >= 60) {
        const int refill = static_cast<int>(elapsed / 60) * config_.rate_limit_per_min;
        tokens_.store(std::min(config_.rate_limit_per_min, tokens_.load() + refill));
        last_refill_.store(now);
    }
}

bool YugipediaClient::ConsumeToken() {
    RefillTokens();
    int current = tokens_.load();
    while (current > 0) {
        if (tokens_.compare_exchange_weak(current, current - 1))
            return true;
    }
    return false;
}

std::string YugipediaClient::PerformRequest(const std::string& url) {
    while (!ConsumeToken())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CURL* curl = curl_easy_init();
    if (!curl) {
        ErrorLog("Failed to initialize Yugipedia request");
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
        ErrorLog("Yugipedia request failed: {}", curl_easy_strerror(result));
        return {};
    }
    if (http_code != 200) {
        ErrorLog("Yugipedia API error: HTTP {}", http_code);
        return {};
    }
    return response;
}

std::optional<CardTranslation> YugipediaClient::FetchTranslation(uint32_t card_id,
                                                                 const std::string& locale,
                                                                 const std::string& card_name) {
    const std::string language = GetYugipediaCode(locale);
    if (language.empty()) {
        ErrorLog("Yugipedia has no language mapping for locale {}", locale);
        return std::nullopt;
    }

    try {
        std::vector<std::string> titles;
        const auto trimmed_name = Trim(card_name);
        if (!trimmed_name.empty())
            titles.push_back(trimmed_name);
        const auto passcode = std::to_string(card_id);
        if (titles.empty() || titles.front() != passcode)
            titles.push_back(passcode);

        bool page_missing = false;
        bool page_mismatch = false;
        for (const auto& title : titles) {
            const std::string url = config_.base_url +
                "?action=query&prop=revisions&rvprop=content&rvslots=main&redirects=1&titles=" +
                UrlEncode(title) + "&format=json&formatversion=2";
            const std::string response = PerformRequest(url);
            if (response.empty())
                return std::nullopt;

            const auto json = nlohmann::json::parse(response);
            if (!json.contains("query") || !json["query"].contains("pages") ||
                !json["query"]["pages"].is_array() || json["query"]["pages"].empty()) {
                ErrorLog("Yugipedia response has no page for card {}", card_id);
                return std::nullopt;
            }

            const auto& page = json["query"]["pages"][0];
            if (page.contains("missing")) {
                page_missing = true;
                continue;
            }
            if (!page.contains("revisions") || !page["revisions"].is_array() ||
                page["revisions"].empty()) {
                ErrorLog("Yugipedia page has no revisions for card {}", card_id);
                return std::nullopt;
            }

            const auto& revision = page["revisions"][0];
            if (!revision.contains("slots") || !revision["slots"].contains("main") ||
                !revision["slots"]["main"].contains("content") ||
                !revision["slots"]["main"]["content"].is_string()) {
                ErrorLog("Yugipedia page has no readable content for card {}", card_id);
                return std::nullopt;
            }

            const auto fields = ParseCardFields(revision["slots"]["main"]["content"].get<std::string>());
            const auto password = fields.find("password");
            const std::regex passcode_pattern("(^|[^0-9])0*" + passcode + "([^0-9]|$)");
            if (password == fields.end() ||
                !std::regex_search(password->second, passcode_pattern)) {
                page_mismatch = true;
                ErrorLog("Yugipedia page '{}' does not match card passcode {}", title, card_id);
                continue;
            }

            CardTranslation translation;
            translation.card_id = card_id;
            translation.locale = locale;
            translation.source = "yugipedia:title:" + language;
            translation.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            const auto name = fields.find(language + "_name");
            if (name != fields.end())
                translation.name = StripWikitext(name->second);
            const auto desc = fields.find(language + "_text");
            if (desc != fields.end())
                translation.desc = StripWikitext(desc->second);

            if (translation.desc.empty())
                translation.source += ":missing";
            return translation;
        }

        if (page_mismatch || !page_missing)
            return std::nullopt;

        ErrorLog("Yugipedia has no page for card {} ({})", card_id, trimmed_name);
        CardTranslation missing;
        missing.card_id = card_id;
        missing.locale = locale;
        missing.source = "yugipedia:title:" + language + ":missing";
        missing.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return missing;
    } catch (const std::exception& e) {
        ErrorLog("Failed to parse Yugipedia response for card {}: {}", card_id, e.what());
        return std::nullopt;
    }
}

} // namespace ygo
