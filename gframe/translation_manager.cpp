#include "translation_manager.h"
#include "data_manager.h"
#include "utils.h"
#include "logging.h"
#include "fmt.h"
#include "bufferio.h"
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <filesystem>

namespace ygo {

namespace {

bool IsObsoleteApertiumTranslation(const CardTranslation& translation) {
    return translation.source.compare(0, 8, "apertium") == 0;
}

bool IsYugipediaMissingTranslation(const CardTranslation& translation) {
    return translation.source.rfind("yugipedia:title:", 0) == 0 &&
        translation.source.find(":missing") != std::string::npos;
}

} // namespace

TranslationManager::TranslationManager() {
}

TranslationManager::~TranslationManager() {
    worker_running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool TranslationManager::Initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        return true;
    }
    
    config_ = config;
    current_locale_ = config.locale;
    
    // Initialize cache
    cache_ = std::make_unique<TranslationCache>();
    std::string cache_path = "./config/translation_cache.db";
    if (!cache_->Initialize(cache_path)) {
        ErrorLog("Failed to initialize translation cache");
        return false;
    }
    
    // Initialize YGOPRODeck client
    ygoprodeck_ = std::make_unique<YGOPRODeckClient>();
    YGOPRODeckClient::Config ygo_config;
    ygo_config.rate_limit_per_sec = config_.ygoprodeck_rate_limit;
    ygo_config.timeout_seconds = 10;
    ygoprodeck_->Initialize(ygo_config);

    yugipedia_ = std::make_unique<YugipediaClient>();
    YugipediaClient::Config yugipedia_config;
    yugipedia_config.rate_limit_per_min = config_.yugipedia_rate_limit_per_min;
    yugipedia_->Initialize(yugipedia_config);
    
    // Ensure locale DB directory exists
    Utils::MakeDirectory(Utils::ToPathString(config_.locale_db_path));
    
    // Start background worker
    worker_running_ = true;
    worker_thread_ = std::thread(&TranslationManager::WorkerLoop, this);
    
    initialized_ = true;
    epro::print("TranslationManager initialized for locale: {}\n", current_locale_);
    
    return true;
}

void TranslationManager::SetLocale(const std::string& locale) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (current_locale_ != locale) {
        current_locale_ = locale;
        config_.locale = locale;
        epro::print("TranslationManager locale changed to: {}\n", locale);
    }
}

std::string TranslationManager::GetLocale() const {
    return current_locale_;
}

bool TranslationManager::IsEnabled() const {
    return config_.enabled && initialized_;
}

void TranslationManager::SetEnabled(bool enabled) {
    config_.enabled = enabled;
}

bool TranslationManager::EnsureCardTranslated(uint32_t card_code, const std::string& locale) {
    if (!IsEnabled()) return false;
    
    std::string target_locale = locale.empty() ? current_locale_ : locale;
    
    // Skip English (base language)
    if (target_locale == "en") return true;
    
    std::optional<CardTranslation> partial_translation;
    bool obsolete_provider_translation = false;

    // 1. Try cache first (fastest)
    if (auto cached = TryGetFromCache(card_code, target_locale)) {
        if (IsObsoleteApertiumTranslation(*cached)) {
            obsolete_provider_translation = true;
        } else if (IsYugipediaMissingTranslation(*cached)) {
            return true;
        } else {
            cache_hits_++;
            if (!cached->desc.empty()) {
                SaveToLocaleDB(*cached);
                return true;
            }
            partial_translation = std::move(cached);
        }
    }
    else {
        cache_misses_++;
    }
    
    // 2. Try locale DB
    if (!obsolete_provider_translation) {
        if (auto from_db = TryGetFromLocaleDB(card_code, target_locale)) {
            if (!from_db->desc.empty()) {
                cache_->Put(*from_db);
                return true;
            }
            partial_translation = std::move(from_db);
        }
    }
    
    // 3. Try YGOPRODeck (if supported)
    if (ygoprodeck_ && IsYGOPRODeckSupported(target_locale)) {
        if (auto from_api = TryFetchFromYGOPRODeck(card_code, target_locale)) {
            ygoprodeck_calls_++;
            if (!from_api->desc.empty()) {
                cache_->Put(*from_api);
                SaveToLocaleDB(*from_api);
                translations_done_++;
                locale_db_writes_++;
                return true;
            }
            partial_translation = std::move(from_api);
        }
    }
    
    // 4. Try the community-maintained Yugipedia text when the official API has no description.
    if (yugipedia_ && IsYugipediaSupported(target_locale)) {
        if (auto translated = TryTranslateWithYugipedia(card_code, target_locale)) {
            if (partial_translation) {
                if (translated->name.empty())
                    translated->name = partial_translation->name;
                for (size_t i = 0; i < translated->strs.size(); ++i) {
                    if (translated->strs[i].empty())
                        translated->strs[i] = partial_translation->strs[i];
                }
            }
            cache_->Put(*translated);
            if (!IsYugipediaMissingTranslation(*translated)) {
                SaveToLocaleDB(*translated);
                translations_done_++;
                locale_db_writes_++;
            }
            return true;
        }
    }

    if (partial_translation && !obsolete_provider_translation) {
        cache_->Put(*partial_translation);
        SaveToLocaleDB(*partial_translation);
        return true;
    }
    
    errors_++;
    return false;
}

std::optional<CardTranslation> TranslationManager::GetTranslation(uint32_t card_code, const std::string& locale) {
    if (!IsEnabled()) return std::nullopt;
    
    std::string target_locale = locale.empty() ? current_locale_ : locale;
    
    if (target_locale == "en") return std::nullopt;
    
    // Try cache
    const auto cached = TryGetFromCache(card_code, target_locale);
    if (cached) {
        if (!IsObsoleteApertiumTranslation(*cached))
            return cached;
    } else {
        if (auto from_db = TryGetFromLocaleDB(card_code, target_locale))
            return from_db;
    }
    
    return std::nullopt;
}

bool TranslationManager::ForceTranslateCard(uint32_t card_code, const std::string& locale) {
    if (!IsEnabled()) return false;
    
    std::string target_locale = locale.empty() ? current_locale_ : locale;
    
    if (target_locale == "en") return true;

    std::optional<CardTranslation> partial_translation;
    
    // Try YGOPRODeck first
    if (ygoprodeck_ && IsYGOPRODeckSupported(target_locale)) {
        if (auto from_api = TryFetchFromYGOPRODeck(card_code, target_locale)) {
            if (!from_api->desc.empty()) {
                cache_->Put(*from_api);
                SaveToLocaleDB(*from_api);
                translations_done_++;
                locale_db_writes_++;
                return true;
            }
            partial_translation = std::move(from_api);
        }
    }
    
    // Fallback to the community-maintained Yugipedia text.
    if (yugipedia_ && IsYugipediaSupported(target_locale)) {
        if (auto translated = TryTranslateWithYugipedia(card_code, target_locale)) {
            if (partial_translation && translated->name.empty())
                translated->name = partial_translation->name;
            cache_->Put(*translated);
            if (!IsYugipediaMissingTranslation(*translated)) {
                SaveToLocaleDB(*translated);
                translations_done_++;
                locale_db_writes_++;
            }
            return true;
        }
    }

    if (partial_translation) {
        cache_->Put(*partial_translation);
        SaveToLocaleDB(*partial_translation);
        return true;
    }
    
    return false;
}

size_t TranslationManager::TranslateCards(const std::vector<uint32_t>& card_codes, const std::string& locale) {
    if (!IsEnabled() || card_codes.empty()) return 0;
    
    std::string target_locale = locale.empty() ? current_locale_ : locale;
    if (target_locale == "en") return 0;
    
    size_t success_count = 0;
    
    for (uint32_t code : card_codes) {
        if (EnsureCardTranslated(code, target_locale)) {
            success_count++;
        }
    }
    
    return success_count;
}

void TranslationManager::OnCardViewed(uint32_t card_code, const std::string& locale) {
    if (!IsEnabled()) return;
    
    // Queue for background translation if not already cached
    std::string target_locale = locale.empty() ? current_locale_ : locale;
    if (target_locale == "en" || target_locale == "English") return;
    
    // Retry partial results, but keep a negative Yugipedia result cached.
    auto cached = TryGetFromCache(card_code, target_locale);
    if (!cached || (cached->desc.empty() && !IsYugipediaMissingTranslation(*cached))) {
        QueueTranslation(card_code, target_locale);
    }
}

std::vector<uint32_t> TranslationManager::TakeCompletedTranslations(const std::string& locale) {
    std::vector<uint32_t> completed;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto it = completed_translations_.begin(); it != completed_translations_.end();) {
        if (it->second == locale) {
            completed.push_back(it->first);
            it = completed_translations_.erase(it);
        } else {
            ++it;
        }
    }
    return completed;
}

void TranslationManager::QueueTranslation(uint32_t card_code, const std::string& locale) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    const auto request = std::make_pair(card_code, locale);
    if (std::find(pending_translations_.begin(), pending_translations_.end(), request) != pending_translations_.end())
        return;
    if (pending_translations_.size() >= 64)
        return;
    pending_translations_.push_back(request);
    translation_queue_.push_back(request);
}

void TranslationManager::WorkerLoop() {
    while (worker_running_) {
        std::optional<std::pair<uint32_t, std::string>> request;
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!translation_queue_.empty()) {
                request = std::move(translation_queue_.front());
                translation_queue_.erase(translation_queue_.begin());
            }
        }
        
        if (request && worker_running_) {
            const auto [code, locale] = *request;
            const bool translated = EnsureCardTranslated(code, locale);
            std::lock_guard<std::mutex> lock(queue_mutex_);
            const auto pending = std::find(pending_translations_.begin(), pending_translations_.end(), *request);
            if (pending != pending_translations_.end())
                pending_translations_.erase(pending);
            if (translated)
                completed_translations_.push_back(*request);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::optional<CardTranslation> TranslationManager::TryGetFromCache(uint32_t card_code, const std::string& locale) {
    if (!cache_) return std::nullopt;
    return cache_->Get(card_code, locale);
}

std::optional<CardTranslation> TranslationManager::TryGetFromLocaleDB(uint32_t card_code, const std::string& locale) {
    std::string db_path = GetLocaleDBPath(locale);
    if (!Utils::FileExists(Utils::ToPathString(db_path))) {
        return std::nullopt;
    }
    
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    
    const char* sql = "SELECT name, desc, str1, str2, str3, str4, str5, str6, str7, str8, "
                      "str9, str10, str11, str12, str13, str14, str15, str16 "
                      "FROM texts WHERE id = ?";
    
    sqlite3_stmt* stmt = nullptr;
    std::optional<CardTranslation> result;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, static_cast<int>(card_code));
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            CardTranslation t;
            t.card_id = card_code;
            t.locale = locale;
            t.source = "locale_db";
            
            const unsigned char* name_ptr = sqlite3_column_text(stmt, 0);
            t.name = name_ptr ? reinterpret_cast<const char*>(name_ptr) : "";
            
            const unsigned char* desc_ptr = sqlite3_column_text(stmt, 1);
            t.desc = desc_ptr ? reinterpret_cast<const char*>(desc_ptr) : "";
            
            for (int i = 0; i < 16; ++i) {
                const unsigned char* str_ptr = sqlite3_column_text(stmt, 2 + i);
                t.strs[i] = str_ptr ? reinterpret_cast<const char*>(str_ptr) : "";
            }
            
            t.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            if (t.HasContent()) {
                result = std::move(t);
            }
        }
        sqlite3_finalize(stmt);
    }
    
    sqlite3_close(db);
    return result;
}

std::optional<CardTranslation> TranslationManager::TryFetchFromYGOPRODeck(uint32_t card_code, const std::string& locale) {
    if (!ygoprodeck_) return std::nullopt;
    
    std::string ygo_lang = GetYGOPRODeckCode(locale);
    if (ygo_lang.empty()) return std::nullopt;
    
    auto card_data = ygoprodeck_->FetchCard(card_code, ygo_lang);
    if (!card_data) return std::nullopt;
    
    return YGOPRODeckClient::ToCardTranslation(*card_data, locale);
}

std::optional<CardTranslation> TranslationManager::TryTranslateWithYugipedia(uint32_t card_code, const std::string& locale) {
    if (!yugipedia_) return std::nullopt;
    yugipedia_calls_++;
    std::string card_name;
    if (gDataManager) {
        const auto card = gDataManager->cards.find(card_code);
        if (card != gDataManager->cards.end())
            card_name = Utils::ToUTF8IfNeeded(card->second._strings.name);
    }
    return yugipedia_->FetchTranslation(card_code, locale, card_name);
}

bool TranslationManager::SaveToLocaleDB(const CardTranslation& translation) {
    std::string db_path = GetLocaleDBPath(translation.locale);
    
    if (!EnsureLocaleDBExists(translation.locale)) {
        return false;
    }
    
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        ErrorLog("Failed to open locale DB: {}", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return false;
    }
    
    const char* sql = R"(
        INSERT OR REPLACE INTO texts 
        (id, name, desc, str1, str2, str3, str4, str5, str6, str7, str8,
         str9, str10, str11, str12, str13, str14, str15, str16)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, static_cast<int>(translation.card_id));
        sqlite3_bind_text(stmt, 2, translation.name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, translation.desc.c_str(), -1, SQLITE_STATIC);
        
        for (int i = 0; i < 16; ++i) {
            sqlite3_bind_text(stmt, 4 + i, translation.strs[i].c_str(), -1, SQLITE_STATIC);
        }
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            success = true;
        }
        sqlite3_finalize(stmt);
    } else {
        ErrorLog("Failed to prepare statement: {}", sqlite3_errmsg(db));
    }
    
    sqlite3_close(db);
    return success;
}

bool TranslationManager::EnsureLocaleDBExists(const std::string& locale) {
    std::string db_path = GetLocaleDBPath(locale);
    std::string dir = Utils::GetFilePath(db_path);
    
    if (!dir.empty()) {
        Utils::MakeDirectory(Utils::ToPathString(dir));
    }
    
    if (Utils::FileExists(Utils::ToPathString(db_path))) {
        return true;
    }
    
    // Create new locale DB with texts table
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        ErrorLog("Failed to create locale DB: {}", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return false;
    }
    
    const char* sql = R"(
        CREATE TABLE texts (
            id INTEGER PRIMARY KEY,
            name TEXT,
            desc TEXT,
            str1 TEXT, str2 TEXT, str3 TEXT, str4 TEXT,
            str5 TEXT, str6 TEXT, str7 TEXT, str8 TEXT,
            str9 TEXT, str10 TEXT, str11 TEXT, str12 TEXT,
            str13 TEXT, str14 TEXT, str15 TEXT, str16 TEXT
        );
        CREATE INDEX idx_texts_id ON texts(id);
    )";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        ErrorLog("Failed to create locale DB tables: {}", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }
    
    sqlite3_close(db);
    epro::print("Created new locale DB: {}\n", db_path);
    return true;
}

bool TranslationManager::SaveLocaleStrings(const std::string& locale) {
    std::string target_locale = locale.empty() ? current_locale_ : locale;
    if (target_locale == "en") return true;
    
    // Read main strings.conf
    std::string main_strings = "./config/strings.conf";
    if (!Utils::FileExists(Utils::ToPathString(main_strings))) {
        main_strings = "./expansions/strings.conf";
        if (!Utils::FileExists(Utils::ToPathString(main_strings))) {
            return false;
        }
    }
    
    // Parse and translate strings.conf entries
    // This would be a more complex implementation
    // For now, just ensure the locale directory exists
    std::string locale_dir = config_.locale_db_path + target_locale;
    Utils::MakeDirectory(Utils::ToPathString(locale_dir));
    
    // Copy main strings.conf to locale directory as starting point
    std::string target_strings = locale_dir + "/strings.conf";
    // In a full implementation, we'd translate each string entry
    
    return true;
}

size_t TranslationManager::CleanupCache() {
    if (!cache_) return 0;
    return cache_->Cleanup(config_.cache_max_age_days);
}

TranslationManager::Stats TranslationManager::GetStats() const {
    Stats stats;
    stats.cache_hits = cache_hits_.load();
    stats.cache_misses = cache_misses_.load();
    stats.ygoprodeck_calls = ygoprodeck_calls_.load();
    stats.yugipedia_calls = yugipedia_calls_.load();
    stats.locale_db_writes = locale_db_writes_.load();
    stats.translations_done = translations_done_.load();
    stats.errors = errors_.load();
    return stats;
}

TranslationCache::Stats TranslationManager::GetCacheStats() const {
    if (cache_) return cache_->GetStats();
    return {};
}

std::string TranslationManager::GetLocaleDBPath(const std::string& locale) const {
    return epro::format("{}/{}/cards.cdb", config_.locale_db_path, locale);
}

std::string TranslationManager::GetLocaleStringsPath(const std::string& locale) const {
    return epro::format("{}/{}/strings.conf", config_.locale_db_path, locale);
}

} // namespace ygo