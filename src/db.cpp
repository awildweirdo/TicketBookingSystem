#include "db.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

DB::DB(const std::string &path)
{
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK)
    {
        std::cerr << "Failed to open sqlite database: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 5000);
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
}

DB::~DB()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool DB::init_schema(const std::string &schema_path, std::string &error_message)
{
    if (!db_)
    {
        error_message = "Database not opened";
        return false;
    }

    if (!std::filesystem::exists(schema_path))
    {
        error_message = "Schema file not found: " + schema_path;
        return false;
    }

    std::ifstream in(schema_path);
    if (!in)
    {
        error_message = "Failed to open schema file: " + schema_path;
        return false;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string sql = buffer.str();

    char *err = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        error_message = err ? err : "Unknown sqlite error";
        if (err)
            sqlite3_free(err);
        return false;
    }
    return true;
}
