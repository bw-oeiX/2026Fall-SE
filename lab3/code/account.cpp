#include "app.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace ticket {
namespace {

std::string hex_encode(const unsigned char* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<int>(data[index]);
    }
    return output.str();
}

std::vector<unsigned char> hex_decode(const std::string& value) {
    if (value.size() % 2 != 0) {
        throw BusinessError("密码摘要格式错误");
    }
    std::vector<unsigned char> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const int byte = std::stoi(value.substr(index, 2), nullptr, 16);
        result.push_back(static_cast<unsigned char>(byte));
    }
    return result;
}

void validate_password(const std::string& password) {
    bool letter = false;
    bool digit = false;
    for (unsigned char ch : password) {
        letter = letter || std::isalpha(ch);
        digit = digit || std::isdigit(ch);
    }
    if (password.size() < 8 || !letter || !digit) {
        throw BusinessError("密码至少 8 位，且同时包含字母和数字");
    }
}

std::string password_hash(const std::string& password) {
    std::array<unsigned char, 16> salt{};
    std::array<unsigned char, 32> output{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw BusinessError("无法生成密码盐值");
    }
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          120000, EVP_sha256(), static_cast<int>(output.size()),
                          output.data()) != 1) {
        throw BusinessError("密码派生失败");
    }
    return hex_encode(salt.data(), salt.size()) + ":" +
           hex_encode(output.data(), output.size());
}

bool password_matches(const std::string& password, const std::string& saved) {
    const std::size_t separator = saved.find(':');
    if (separator == std::string::npos) {
        return false;
    }
    const auto salt = hex_decode(saved.substr(0, separator));
    const auto expected = hex_decode(saved.substr(separator + 1));
    std::vector<unsigned char> actual(expected.size());
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          120000, EVP_sha256(), static_cast<int>(actual.size()),
                          actual.data()) != 1) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        difference |= expected[index] ^ actual[index];
    }
    return difference == 0;
}

void validate_account_fields(const std::string& username,
                             const std::string& phone,
                             const std::string& password) {
    if (username.empty() || username.size() > 64 || phone.empty() ||
        phone.size() > 32) {
        throw BusinessError("用户名或手机号为空或过长");
    }
    validate_password(password);
}

}  // namespace

App::App(const std::filesystem::path& database_file) : db_(database_file) {}

void App::set_test_time(Time value) {
    test_time_ = value;
}

void App::use_system_time() {
    test_time_.reset();
}

Time App::now() const {
    if (test_time_) {
        return *test_time_;
    }
    const auto current = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
               current.time_since_epoch()).count();
}

User App::read_user(Statement& statement) {
    User result;
    result.id = statement.integer64(0);
    result.username = statement.text(1);
    result.phone = statement.text(2);
    result.role = statement.text(3);
    result.must_change_password = statement.integer(4) != 0;
    result.restricted = statement.integer(5) != 0;
    return result;
}

User App::user(Id id) {
    auto query = db_.prepare(
        "SELECT id, username, phone, role, must_change_password, restricted "
        "FROM users WHERE id = ?");
    query.bind(1, id);
    if (!query.step()) {
        throw BusinessError("用户不存在");
    }
    return read_user(query);
}

User App::require_role(Id id, const std::string& role) {
    const User actor = user(id);
    if (actor.role != role) {
        throw BusinessError("该角色无权执行此操作");
    }
    if (actor.must_change_password) {
        throw BusinessError("首次登录须先修改临时密码");
    }
    return actor;
}

void App::require_not_restricted(const User& actor) {
    if (actor.restricted) {
        throw BusinessError("账号相应权限已被管理员限制");
    }
}

Id App::bootstrap_admin(const std::string& username, const std::string& phone,
                        const std::string& password) {
    auto count = db_.prepare("SELECT COUNT(*) FROM users WHERE role = 'admin'");
    count.step();
    if (count.integer(0) != 0) {
        throw BusinessError("已有管理员，不能再次初始化");
    }
    validate_account_fields(username, phone, password);
    Transaction transaction(db_);
    auto insert = db_.prepare(
        "INSERT INTO users(username,phone,password_hash,role,must_change_password,created_at) "
        "VALUES(?,?,?,'admin',1,?)");
    insert.bind(1, username).bind(2, phone)
        .bind(3, password_hash(password)).bind(4, now()).execute();
    const Id id = db_.last_id();
    audit_write(id, 0, "user", id, "bootstrap_admin", "", "created");
    transaction.commit();
    return id;
}

Id App::register_user(const std::string& role, const std::string& username,
                      const std::string& phone, const std::string& password) {
    if (role != "participant" && role != "organizer") {
        throw BusinessError("访客只能注册参与者或活动组织者");
    }
    validate_account_fields(username, phone, password);
    auto existing = db_.prepare(
        "SELECT username,phone FROM users WHERE username=? OR phone=? LIMIT 1");
    existing.bind(1, username).bind(2, phone);
    if (existing.step()) {
        throw BusinessError(existing.text(1) == phone ? "手机号已注册" : "用户名已被使用");
    }
    Transaction transaction(db_);
    auto insert = db_.prepare(
        "INSERT INTO users(username,phone,password_hash,role,created_at) "
        "VALUES(?,?,?,?,?)");
    insert.bind(1, username).bind(2, phone)
        .bind(3, password_hash(password)).bind(4, role)
        .bind(5, now()).execute();
    const Id id = db_.last_id();
    audit_write(id, 0, "user", id, "register", "", role);
    transaction.commit();
    return id;
}

Id App::create_staff(Id admin_id, const std::string& role,
                     const std::string& username, const std::string& phone,
                     const std::string& temporary_password) {
    require_role(admin_id, "admin");
    if (role != "checker" && role != "admin") {
        throw BusinessError("仅能创建检票人员或管理员账号");
    }
    validate_account_fields(username, phone, temporary_password);
    Transaction transaction(db_);
    auto insert = db_.prepare(
        "INSERT INTO users(username,phone,password_hash,role,must_change_password,created_at) "
        "VALUES(?,?,?,?,1,?)");
    insert.bind(1, username).bind(2, phone)
        .bind(3, password_hash(temporary_password)).bind(4, role)
        .bind(5, now()).execute();
    const Id id = db_.last_id();
    audit_write(admin_id, 0, "user", id, "create_staff", "", role);
    transaction.commit();
    return id;
}

User App::login(const std::string& role, const std::string& username,
                const std::string& phone, const std::string& password) {
    auto query = db_.prepare(
        "SELECT id,username,phone,role,must_change_password,restricted,password_hash "
        "FROM users WHERE role=? AND username=? AND phone=?");
    query.bind(1, role).bind(2, username).bind(3, phone);
    if (!query.step() || !password_matches(password, query.text(6))) {
        throw BusinessError("登录身份、用户名、手机号或密码不正确");
    }
    return read_user(query);
}

void App::change_password(Id actor_id, const std::string& old_password,
                          const std::string& new_password) {
    validate_password(new_password);
    auto query = db_.prepare("SELECT password_hash FROM users WHERE id=?");
    query.bind(1, actor_id);
    if (!query.step() || !password_matches(old_password, query.text(0))) {
        throw BusinessError("当前密码不正确");
    }
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE users SET password_hash=?,must_change_password=0 WHERE id=?");
    update.bind(1, password_hash(new_password)).bind(2, actor_id).execute();
    audit_write(actor_id, 0, "user", actor_id, "change_password", "old hash",
                "new hash");
    transaction.commit();
}

void App::reset_password(Id admin_id, Id target_id,
                         const std::string& temporary_password) {
    require_role(admin_id, "admin");
    user(target_id);
    validate_password(temporary_password);
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE users SET password_hash=?,must_change_password=1 WHERE id=?");
    update.bind(1, password_hash(temporary_password)).bind(2, target_id).execute();
    audit_write(admin_id, 0, "user", target_id, "reset_password", "old hash",
                "temporary hash");
    transaction.commit();
}

void App::set_restriction(Id admin_id, Id target_id, bool restricted,
                          const std::string& reason) {
    require_role(admin_id, "admin");
    const User target = user(target_id);
    if (target.role != "participant" && target.role != "organizer") {
        throw BusinessError("仅能限制参与者或组织者业务权限");
    }
    if (reason.empty()) {
        throw BusinessError("限制或解除权限必须说明原因");
    }
    Transaction transaction(db_);
    auto update = db_.prepare("UPDATE users SET restricted=? WHERE id=?");
    update.bind(1, restricted ? 1 : 0).bind(2, target_id).execute();
    audit_write(admin_id, 0, "user", target_id, "set_restriction",
                target.restricted ? "restricted" : "normal",
                restricted ? "restricted" : "normal", "success", reason);
    notice(target_id, 0, restricted ? "管理员已限制相应业务权限" : "管理员已解除权限限制");
    transaction.commit();
}

void App::audit_write(Id actor_id, Id event_id, const std::string& object_type,
                      Id object_id, const std::string& action,
                      const std::string& before, const std::string& after,
                      const std::string& result, const std::string& reason,
                      const std::string& request_id) {
    auto insert = db_.prepare(
        "INSERT INTO audit_logs(actor_id,event_id,object_type,object_id,action,"
        "before_value,after_value,result,reason,request_id,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    if (actor_id == 0) insert.bind_null(1); else insert.bind(1, actor_id);
    if (event_id == 0) insert.bind_null(2); else insert.bind(2, event_id);
    insert.bind(3, object_type);
    if (object_id == 0) insert.bind_null(4); else insert.bind(4, object_id);
    insert.bind(5, action).bind(6, before).bind(7, after)
        .bind(8, result).bind(9, reason).bind(10, request_id)
        .bind(11, now()).execute();
}

void App::notice(Id user_id, Id event_id, const std::string& message) {
    auto insert = db_.prepare(
        "INSERT INTO notices(user_id,event_id,message,created_at) VALUES(?,?,?,?)");
    insert.bind(1, user_id);
    if (event_id == 0) insert.bind_null(2); else insert.bind(2, event_id);
    insert.bind(3, message).bind(4, now()).execute();
}

std::vector<std::string> App::notices(Id user_id) {
    user(user_id);
    auto query = db_.prepare(
        "SELECT message FROM notices WHERE user_id=? ORDER BY id DESC LIMIT 100");
    query.bind(1, user_id);
    std::vector<std::string> result;
    while (query.step()) {
        result.push_back(query.text(0));
    }
    return result;
}

}  // namespace ticket
