#include "db.hpp"

#include <utility>

namespace ticket {

namespace {

void check(int code, sqlite3* connection, const std::string& operation) {
    if (code != SQLITE_OK) {
        throw DatabaseError(operation + ": " + sqlite3_errmsg(connection));
    }
}

}  // namespace

DatabaseError::DatabaseError(const std::string& message)
    : std::runtime_error(message) {}

Statement::Statement(sqlite3* connection, const std::string& sql)
    : connection_(connection) {
    const int code = sqlite3_prepare_v2(connection_, sql.c_str(), -1, &statement_, nullptr);
    check(code, connection_, "SQL 预编译失败");
}

Statement::~Statement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

Statement::Statement(Statement&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)),
      statement_(std::exchange(other.statement_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
        connection_ = std::exchange(other.connection_, nullptr);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

Statement& Statement::bind(int index, int value) {
    check(sqlite3_bind_int(statement_, index, value), connection_, "整数绑定失败");
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    check(sqlite3_bind_int64(statement_, index, value), connection_, "整数绑定失败");
    return *this;
}

Statement& Statement::bind(int index, double value) {
    check(sqlite3_bind_double(statement_, index, value), connection_, "实数绑定失败");
    return *this;
}

Statement& Statement::bind(int index, const std::string& value) {
    check(sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT),
          connection_, "文本绑定失败");
    return *this;
}

Statement& Statement::bind_null(int index) {
    check(sqlite3_bind_null(statement_, index), connection_, "空值绑定失败");
    return *this;
}

bool Statement::step() {
    const int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) {
        return true;
    }
    if (code == SQLITE_DONE) {
        return false;
    }
    throw DatabaseError("SQL 执行失败: " + std::string(sqlite3_errmsg(connection_)));
}

void Statement::execute() {
    if (step()) {
        throw DatabaseError("预期写入语句却返回了查询结果");
    }
}

void Statement::reset() {
    check(sqlite3_reset(statement_), connection_, "语句重置失败");
    check(sqlite3_clear_bindings(statement_), connection_, "参数清除失败");
}

int Statement::integer(int column) const {
    return sqlite3_column_int(statement_, column);
}

std::int64_t Statement::integer64(int column) const {
    return sqlite3_column_int64(statement_, column);
}

double Statement::real(int column) const {
    return sqlite3_column_double(statement_, column);
}

std::string Statement::text(int column) const {
    const unsigned char* value = sqlite3_column_text(statement_, column);
    return value == nullptr ? "" : reinterpret_cast<const char*>(value);
}

bool Statement::is_null(int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

Database::Database(const std::filesystem::path& file) {
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path());
    }
    const std::string name = file.u8string();
    const int code = sqlite3_open_v2(
        name.c_str(), &connection_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (code != SQLITE_OK) {
        const std::string message = connection_ == nullptr
                                        ? "数据库打开失败"
                                        : sqlite3_errmsg(connection_);
        if (connection_ != nullptr) {
            sqlite3_close(connection_);
        }
        throw DatabaseError(message);
    }
    sqlite3_busy_timeout(connection_, 5000);
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA journal_mode = WAL");
    initialize();
}

Database::~Database() {
    if (connection_ != nullptr) {
        sqlite3_close(connection_);
    }
}

Statement Database::prepare(const std::string& sql) {
    return Statement(connection_, sql);
}

void Database::exec(const std::string& sql) {
    char* error = nullptr;
    const int code = sqlite3_exec(connection_, sql.c_str(), nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        const std::string message = error == nullptr ? sqlite3_errmsg(connection_) : error;
        sqlite3_free(error);
        throw DatabaseError("SQL 执行失败: " + message);
    }
}

std::int64_t Database::last_id() const {
    return sqlite3_last_insert_rowid(connection_);
}

int Database::changes() const {
    return sqlite3_changes(connection_);
}

sqlite3* Database::handle() const {
    return connection_;
}

Transaction::Transaction(Database& database) : database_(database) {
    database_.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (!finished_) {
        try {
            database_.exec("ROLLBACK");
        } catch (...) {
            // 析构函数不能抛异常；原始错误仍交给调用者。
        }
    }
}

void Transaction::commit() {
    if (!finished_) {
        database_.exec("COMMIT");
        finished_ = true;
    }
}

void Transaction::rollback() {
    if (!finished_) {
        database_.exec("ROLLBACK");
        finished_ = true;
    }
}

void Database::initialize() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    phone TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL CHECK(role IN ('participant','organizer','checker','admin')),
    must_change_password INTEGER NOT NULL DEFAULT 0,
    restricted INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS categories (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    enabled INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY,
    organizer_id INTEGER NOT NULL REFERENCES users(id),
    category_id INTEGER NOT NULL REFERENCES categories(id),
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    location TEXT NOT NULL,
    start_at INTEGER NOT NULL,
    end_at INTEGER NOT NULL,
    sale_start INTEGER NOT NULL,
    sale_end INTEGER NOT NULL,
    price_cents INTEGER NOT NULL CHECK(price_cents BETWEEN 0 AND 99999900),
    capacity INTEGER NOT NULL CHECK(capacity BETWEEN 1 AND 9999),
    occupied INTEGER NOT NULL DEFAULT 0 CHECK(occupied >= 0 AND occupied <= capacity),
    status TEXT NOT NULL CHECK(status IN ('draft','under_review','published','cancelled','removed')),
    review_reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    CHECK(start_at < end_at),
    CHECK(sale_start < sale_end),
    CHECK(sale_end <= start_at)
);
CREATE TABLE IF NOT EXISTS registrations (
    id INTEGER PRIMARY KEY,
    event_id INTEGER NOT NULL REFERENCES events(id),
    participant_id INTEGER NOT NULL REFERENCES users(id),
    status TEXT NOT NULL CHECK(status IN
      ('waitlisted','pending_payment','purchased','cancelled','checked_in','absent','invalid')),
    queue_seq INTEGER NOT NULL,
    request_id TEXT NOT NULL UNIQUE,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS one_active_registration
    ON registrations(event_id, participant_id)
    WHERE status IN ('waitlisted','pending_payment','purchased','checked_in');
CREATE INDEX IF NOT EXISTS waitlist_order
    ON registrations(event_id, status, queue_seq);
CREATE TABLE IF NOT EXISTS orders (
    id INTEGER PRIMARY KEY,
    registration_id INTEGER NOT NULL UNIQUE REFERENCES registrations(id),
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL CHECK(status IN
      ('pending_payment','paid','cancelled','refunded','timed_out')),
    pay_deadline INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS tickets (
    id INTEGER PRIMARY KEY,
    order_id INTEGER NOT NULL UNIQUE REFERENCES orders(id),
    code TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK(status IN ('valid','checked','invalid')),
    checked_at INTEGER
);
CREATE TABLE IF NOT EXISTS reports (
    id INTEGER PRIMARY KEY,
    event_id INTEGER NOT NULL REFERENCES events(id),
    reporter_id INTEGER NOT NULL REFERENCES users(id),
    reason TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('pending','true','false')),
    result_reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    decided_at INTEGER,
    UNIQUE(event_id, reporter_id)
);
CREATE TABLE IF NOT EXISTS audit_logs (
    id INTEGER PRIMARY KEY,
    actor_id INTEGER,
    event_id INTEGER,
    object_type TEXT NOT NULL,
    object_id INTEGER,
    action TEXT NOT NULL,
    before_value TEXT NOT NULL,
    after_value TEXT NOT NULL,
    result TEXT NOT NULL,
    reason TEXT NOT NULL,
    request_id TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS audit_event_time ON audit_logs(event_id, created_at);
CREATE TABLE IF NOT EXISTS notices (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id),
    event_id INTEGER REFERENCES events(id),
    message TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    read_at INTEGER
);
)SQL");
}

}  // namespace ticket
