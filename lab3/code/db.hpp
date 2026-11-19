#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace ticket {

class DatabaseError : public std::runtime_error {
public:
    explicit DatabaseError(const std::string& message);
};

class Statement {
public:
    Statement(sqlite3* connection, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    Statement& bind(int index, int value);
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, double value);
    Statement& bind(int index, const std::string& value);
    Statement& bind_null(int index);

    // true 表示读到一行；false 表示语句执行完毕。
    bool step();
    void execute();
    void reset();

    int integer(int column) const;
    std::int64_t integer64(int column) const;
    double real(int column) const;
    std::string text(int column) const;
    bool is_null(int column) const;

private:
    sqlite3* connection_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class Database {
public:
    explicit Database(const std::filesystem::path& file);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Statement prepare(const std::string& sql);
    void exec(const std::string& sql);
    std::int64_t last_id() const;
    int changes() const;
    sqlite3* handle() const;

    void initialize();

private:
    sqlite3* connection_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();
    void rollback();

private:
    Database& database_;
    bool finished_ = false;
};

}  // namespace ticket
