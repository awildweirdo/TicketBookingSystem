#pragma once

#include <sqlite3.h>
#include <string>

class DB
{
public:
    explicit DB(const std::string &path);
    ~DB();

    // Initialize the database schema using the provided SQL file path.
    // On error, returns false and fills `error_message`.
    bool init_schema(const std::string &schema_path, std::string &error_message);

    sqlite3 *get() const { return db_; }

private:
    sqlite3 *db_{nullptr};
};
