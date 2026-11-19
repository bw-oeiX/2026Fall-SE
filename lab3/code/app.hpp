#pragma once

#include "db.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ticket {

using Id = std::int64_t;
using Time = std::int64_t;

struct User {
    Id id = 0;
    std::string username;
    std::string phone;
    std::string role;
    bool must_change_password = false;
    bool restricted = false;
};

struct Event {
    Id id = 0;
    Id organizer_id = 0;
    Id category_id = 0;
    std::string name;
    std::string description;
    std::string location;
    Time start_at = 0;
    Time end_at = 0;
    Time sale_start = 0;
    Time sale_end = 0;
    int price_cents = 0;
    int capacity = 0;
    int occupied = 0;
    std::string status;
    std::string review_reason;
};

struct Category {
    Id id = 0;
    std::string name;
};

struct EventInput {
    Id category_id = 0;
    std::string name;
    std::string description;
    std::string location;
    Time start_at = 0;
    Time end_at = 0;
    Time sale_start = 0;
    Time sale_end = 0;
    int price_cents = 0;
    int capacity = 0;
};

struct SearchFilter {
    std::optional<Id> category_id;
    std::optional<int> min_price_cents;
    std::optional<int> max_price_cents;
    std::optional<Time> from_time;
    std::optional<Time> to_time;
    std::string location;
    std::string status;
    std::string order_by = "time";
    int page = 1;
    int page_size = 20;
};

struct Booking {
    Id registration_id = 0;
    Id order_id = 0;
    Id event_id = 0;
    Id participant_id = 0;
    std::string registration_status;
    std::string order_status;
    Time pay_deadline = 0;
    std::string ticket_code;
};

struct Stats {
    Id event_id = 0;
    int capacity = 0;
    int remaining = 0;
    int pending_payment = 0;
    int sold = 0;
    int waitlisted = 0;
    int checked_in = 0;
    int absent = 0;
    double attendance_rate = -1;
    double absence_rate = -1;
    double utilization_rate = -1;
};

struct AuditEntry {
    Id id = 0;
    Id actor_id = 0;
    Id event_id = 0;
    std::string object_type;
    Id object_id = 0;
    std::string action;
    std::string before_value;
    std::string after_value;
    std::string result;
    std::string reason;
    std::string request_id;
    Time created_at = 0;
};

struct ParticipantRow {
    std::string username;
    std::string ticket_code;
    std::string status;
};

struct RiskCounts {
    int absent = 0;
    int voluntary_refunds = 0;
    int organizer_violations = 0;
    bool pending_admin_review = false;
};

class BusinessError : public std::runtime_error {
public:
    explicit BusinessError(const std::string& message);
};

class App {
public:
    explicit App(const std::filesystem::path& database_file);

    // 测试可固定时钟；正式运行默认使用系统时钟。
    void set_test_time(Time value);
    void use_system_time();
    Time now() const;

    Id bootstrap_admin(const std::string& username, const std::string& phone,
                       const std::string& password);
    Id register_user(const std::string& role, const std::string& username,
                     const std::string& phone, const std::string& password);
    Id create_staff(Id admin_id, const std::string& role,
                    const std::string& username, const std::string& phone,
                    const std::string& temporary_password);
    User login(const std::string& role, const std::string& username,
               const std::string& phone, const std::string& password);
    User user(Id id);
    void change_password(Id actor_id, const std::string& old_password,
                         const std::string& new_password);
    void reset_password(Id admin_id, Id target_id, const std::string& temporary_password);
    void set_restriction(Id admin_id, Id target_id, bool restricted,
                         const std::string& reason);

    Id add_category(Id admin_id, const std::string& name);
    void disable_category(Id admin_id, Id category_id);
    std::vector<Category> categories();
    Id create_event(Id organizer_id, const EventInput& input);
    void edit_draft(Id organizer_id, Id event_id, const EventInput& input);
    void edit_published_description(Id organizer_id, Id event_id,
                                    const std::string& description);
    void submit_event(Id organizer_id, Id event_id);
    void withdraw_event(Id organizer_id, Id event_id);
    void review_event(Id admin_id, Id event_id, bool approved,
                      const std::string& reason);
    void cancel_event(Id organizer_id, Id event_id);
    Event event(Id event_id);
    std::vector<Event> search_events(const SearchFilter& filter,
                                     std::optional<Id> viewer = std::nullopt);

    Booking book(Id participant_id, Id event_id, const std::string& request_id);
    Booking join_waitlist(Id participant_id, Id event_id,
                          const std::string& request_id);
    Booking booking(Id registration_id, Id viewer_id);
    std::vector<Booking> my_bookings(Id participant_id);
    Booking pay(Id participant_id, Id registration_id, const std::string& request_id);
    void cancel_booking(Id participant_id, Id registration_id,
                        const std::string& request_id);
    void check_in(Id checker_id, Id event_id, const std::string& ticket_code,
                  const std::string& request_id);
    void maintenance();

    Id report_event(Id participant_id, Id event_id, const std::string& reason);
    void decide_report(Id admin_id, Id report_id, bool confirmed,
                       const std::string& reason);
    Stats statistics(Id viewer_id, Id event_id);
    std::vector<ParticipantRow> participant_list(Id viewer_id, Id event_id);
    RiskCounts risk_counts(Id admin_id, Id target_id);
    void export_csv(Id viewer_id, Id event_id, const std::filesystem::path& file,
                    const std::string& kind);
    std::vector<AuditEntry> audit(Id admin_id, Id event_id, int limit = 100);
    std::vector<std::string> notices(Id user_id);

private:
    Database db_;
    std::optional<Time> test_time_;

    static User read_user(Statement& statement);
    static Event read_event(Statement& statement);
    Booking read_booking(Id registration_id);
    User require_role(Id id, const std::string& role);
    void require_not_restricted(const User& actor);
    void validate_event(const EventInput& input);
    void audit_write(Id actor_id, Id event_id, const std::string& object_type,
                     Id object_id, const std::string& action,
                     const std::string& before, const std::string& after,
                     const std::string& result = "success",
                     const std::string& reason = "",
                     const std::string& request_id = "");
    void notice(Id user_id, Id event_id, const std::string& message);
    void promote_waitlist(Id event_id);
    void release_slot(Id event_id);
    void close_event_records(Id event_id, const std::string& final_status);
    void ensure_visible(Id viewer_id, const Event& event);
};

Time parse_beijing_time(const std::string& value);
std::string format_beijing_time(Time value);
std::string format_money(int cents);
std::string csv_escape(const std::string& value);
int utf8_characters(const std::string& value);

}  // namespace ticket
