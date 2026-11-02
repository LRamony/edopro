#include "translation_cache.h"
#include "utils.h"
#include "logging.h"
#include <sqlite3.h>
#include <chrono>
#include <ctime>

namespace ygo {

TranslationCache::TranslationCache() 
    : db_(nullptr, [](sqlite3* p) { if (p) sqlite3_close(p); }) {
}

TranslationCache::~TranslationCache() = default;

bool TranslationCache::Initialize(const std::string& cache_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    cache_path_ = cache_path;
    
    // Ensure directory exists
    std::string dir = Utils::GetFilePath(cache_path_);
    if (!dir.empty()) {
        Utils::MakeDirectory(Utils::ToPathString(dir));
    }
    
    sqlite3* raw_db = nullptr;
    int rc = sqlite3_open_v2(cache_path_.c_str(), &raw_db, 
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 
                             nullptr);
    if (rc != SQLITE_OK) {
        ErrorLog("Failed to open translation cache: {}", sqlite3_errmsg(raw_db));
        if (raw_db) sqlite3_close(raw_db);
        return false;
    }
    
    db_.reset(raw_db);
    
    // Enable WAL mode for better concurrency
    sqlite3_exec(db_.get(), "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_.get(), "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    
    return CreateTables();
}

bool TranslationCache::CreateTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS translations (
            card_id INTEGER NOT NULL,
            locale TEXT NOT NULL,
            source TEXT NOT NULL,
            name TEXT,
            desc TEXT,
            str1 TEXT, str2 TEXT, str3 TEXT, str4 TEXT,
            str5 TEXT, str6 TEXT, str7 TEXT, str8 TEXT,
            str9 TEXT, str10 TEXT, str11 TEXT, str12 TEXT,
            str13 TEXT, str14 TEXT, str15 TEXT, str16 TEXT,
            updated_at INTEGER NOT NULL,
            PRIMARY KEY (card_id, locale)
        );
        
        CREATE TABLE IF NOT EXISTS api_usage (
            date TEXT PRIMARY KEY,
            ygoprodeck_calls INTEGER DEFAULT 0,
            libretranslate_chars INTEGER DEFAULT 0
        );
        
        CREATE INDEX IF NOT EXISTS idx_translations_locale ON translations(locale);
        CREATE INDEX IF NOT EXISTS idx_translations_updated ON translations(updated_at);
    )";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        ErrorLog("Failed to create cache tables: {}", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

int64_t TranslationCache::GetCurrentTimestamp() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string TranslationCache::GetTodayDate() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&time_t));
    return buf;
}

std::optional<CardTranslation> TranslationCache::Get(uint32_t card_id, const std::string& locale) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return std::nullopt;
    
    const char* sql = R"(
        SELECT source, name, desc, 
               str1, str2, str3, str4, str5, str6, str7, str8,
               str9, str10, str11, str12, str13, str14, str15, str16,
               updated_at
        FROM translations 
        WHERE card_id = ? AND locale = ?
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    
    sqlite3_bind_int(stmt, 1, static_cast<int>(card_id));
    sqlite3_bind_text(stmt, 2, locale.c_str(), -1, SQLITE_STATIC);
    
    std::optional<CardTranslation> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        CardTranslation t;
        t.card_id = card_id;
        t.locale = locale;
        
        t.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        
        const unsigned char* name_ptr = sqlite3_column_text(stmt, 1);
        t.name = name_ptr ? reinterpret_cast<const char*>(name_ptr) : "";
        
        const unsigned char* desc_ptr = sqlite3_column_text(stmt, 2);
        t.desc = desc_ptr ? reinterpret_cast<const char*>(desc_ptr) : "";
        
        for (int i = 0; i < 16; ++i) {
            const unsigned char* str_ptr = sqlite3_column_text(stmt, 3 + i);
            t.strs[i] = str_ptr ? reinterpret_cast<const char*>(str_ptr) : "";
        }
        
        t.updated_at = sqlite3_column_int64(stmt, 19);
        result = std::move(t);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

bool TranslationCache::Put(const CardTranslation& translation) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return false;
    
    const char* sql = R"(
        INSERT OR REPLACE INTO translations 
        (card_id, locale, source, name, desc, 
         str1, str2, str3, str4, str5, str6, str7, str8,
         str9, str10, str11, str12, str13, str14, str15, str16,
         updated_at)
        VALUES (?, ?, ?, ?, ?, 
                ?, ?, ?, ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?, ?, ?,
                ?)
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, static_cast<int>(translation.card_id));
    sqlite3_bind_text(stmt, 2, translation.locale.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, translation.source.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, translation.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, translation.desc.c_str(), -1, SQLITE_STATIC);
    
    for (int i = 0; i < 16; ++i) {
        sqlite3_bind_text(stmt, 6 + i, translation.strs[i].c_str(), -1, SQLITE_STATIC);
    }
    
    sqlite3_bind_int64(stmt, 22, translation.updated_at > 0 ? translation.updated_at : GetCurrentTimestamp());
    
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    
    return success;
}

bool TranslationCache::PutBatch(const std::vector<CardTranslation>& translations) {
    if (translations.empty()) return true;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return false;
    
    // Begin transaction
    if (sqlite3_exec(db_.get(), "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }
    
    const char* sql = R"(
        INSERT OR REPLACE INTO translations 
        (card_id, locale, source, name, desc, 
         str1, str2, str3, str4, str5, str6, str7, str8,
         str9, str10, str11, str12, str13, str14, str15, str16,
         updated_at)
        VALUES (?, ?, ?, ?, ?, 
                ?, ?, ?, ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?, ?, ?,
                ?)
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    
    bool all_success = true;
    for (const auto& t : translations) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        
        sqlite3_bind_int(stmt, 1, static_cast<int>(t.card_id));
        sqlite3_bind_text(stmt, 2, t.locale.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, t.source.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, t.name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, t.desc.c_str(), -1, SQLITE_STATIC);
        
        for (int i = 0; i < 16; ++i) {
            sqlite3_bind_text(stmt, 6 + i, t.strs[i].c_str(), -1, SQLITE_STATIC);
        }
        
        sqlite3_bind_int64(stmt, 22, t.updated_at > 0 ? t.updated_at : GetCurrentTimestamp());
        
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            all_success = false;
            break;
        }
    }
    
    sqlite3_finalize(stmt);
    
    if (all_success) {
        sqlite3_exec(db_.get(), "COMMIT;", nullptr, nullptr, nullptr);
    } else {
        sqlite3_exec(db_.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    
    return all_success;
}

bool TranslationCache::Exists(uint32_t card_id, const std::string& locale) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return false;
    
    const char* sql = "SELECT 1 FROM translations WHERE card_id = ? AND locale = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, static_cast<int>(card_id));
    sqlite3_bind_text(stmt, 2, locale.c_str(), -1, SQLITE_STATIC);
    
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

size_t TranslationCache::Cleanup(int max_age_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return 0;
    
    int64_t cutoff = GetCurrentTimestamp() - static_cast<int64_t>(max_age_days) * 86400;
    
    const char* sql = "DELETE FROM translations WHERE updated_at < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_int64(stmt, 1, cutoff);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_.get());
    sqlite3_finalize(stmt);
    
    // Also cleanup old api_usage entries (keep last 90 days)
    int64_t api_cutoff = GetCurrentTimestamp() - 90 * 86400;
    char date_buf[16];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", std::localtime(reinterpret_cast<const time_t*>(&api_cutoff)));
    
    char api_sql[256];
    snprintf(api_sql, sizeof(api_sql), "DELETE FROM api_usage WHERE date < '%s'", date_buf);
    sqlite3_exec(db_.get(), api_sql, nullptr, nullptr, nullptr);
    
    return static_cast<size_t>(changes);
}

TranslationCache::Stats TranslationCache::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Stats stats;
    if (!db_) return stats;
    
    // Count total entries
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), "SELECT COUNT(*) FROM translations", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_entries = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    
    // Get today's API usage
    stats.ygoprodeck_calls_today = GetYGOPRODeckCallsToday();
    stats.libretranslate_chars_today = GetLibreTranslateCharsToday();
    
    return stats;
}

void TranslationCache::IncrementYGOPRODeckCalls(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return;
    
    std::string today = GetTodayDate();
    
    const char* sql = R"(
        INSERT INTO api_usage (date, ygoprodeck_calls, libretranslate_chars)
        VALUES (?, ?, 0)
        ON CONFLICT(date) DO UPDATE SET ygoprodeck_calls = ygoprodeck_calls + ?
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, today.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, count);
        sqlite3_bind_int(stmt, 3, count);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void TranslationCache::IncrementLibreTranslateChars(int chars) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return;
    
    std::string today = GetTodayDate();
    
    const char* sql = R"(
        INSERT INTO api_usage (date, ygoprodeck_calls, libretranslate_chars)
        VALUES (?, 0, ?)
        ON CONFLICT(date) DO UPDATE SET libretranslate_chars = libretranslate_chars + ?
    )";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, today.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, chars);
        sqlite3_bind_int(stmt, 3, chars);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

int TranslationCache::GetYGOPRODeckCallsToday() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return 0;
    
    std::string today = GetTodayDate();
    const char* sql = "SELECT ygoprodeck_calls FROM api_usage WHERE date = ?";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, today.c_str(), -1, SQLITE_STATIC);
    
    int calls = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        calls = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return calls;
}

int TranslationCache::GetLibreTranslateCharsToday() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!db_) return 0;
    
    std::string today = GetTodayDate();
    const char* sql = "SELECT libretranslate_chars FROM api_usage WHERE date = ?";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, today.c_str(), -1, SQLITE_STATIC);
    
    int chars = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        chars = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return chars;
}

} // namespace ygo