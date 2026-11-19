#include "app.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("测试失败: " + message);
    }
}

template <typename Work>
void rejects(Work work, const std::string& message) {
    try {
        work();
    } catch (const ticket::BusinessError&) {
        return;
    } catch (const ticket::DatabaseError&) {
        return;
    }
    throw std::runtime_error("应该拒绝但未拒绝: " + message);
}

ticket::EventInput event_input(ticket::Id category, ticket::Time start,
                               int capacity) {
    ticket::EventInput input;
    input.category_id = category;
    input.name = "活动测试";
    input.description = "仅用于自动测试";
    input.location = "南京";
    input.start_at = start;
    input.end_at = start + 7200;
    input.sale_start = start - 3 * 86400;
    input.sale_end = start - 3600;
    input.price_cents = 12800;
    input.capacity = capacity;
    return input;
}

void core_flow(const std::filesystem::path& file) {
    using namespace ticket;
    const Time base = parse_beijing_time("2026-11-13T10:00");
    App app(file);
    app.set_test_time(base);

    const Id admin = app.bootstrap_admin("admin", "13000000001", "Admin12345");
    check(app.login("admin", "admin", "13000000001", "Admin12345")
              .must_change_password,
          "初始管理员强制改密");
    rejects([&] { app.add_category(admin, "禁止提前添加"); },
            "改密前管理员不能管理类别");
    app.change_password(admin, "Admin12345", "Changed12345");
    rejects([&] { app.login("admin", "admin", "13000000001", "Admin12345"); },
            "旧密码失效");
    check(app.login("admin", "admin", "13000000001", "Changed12345").id == admin,
          "新密码登录");
    rejects([&] { app.login("admin", "admin", "13000000002", "Changed12345"); },
            "手机号与用户名必须同时匹配");

    const Id organizer = app.register_user(
        "organizer", "organizer", "13000000002", "Org12345");
    const Id alice = app.register_user(
        "participant", "alice", "13000000003", "Alice12345");
    const Id bob = app.register_user(
        "participant", "bob", "13000000004", "Bob12345");
    rejects([&] { app.register_user("participant", "another",
                                     "13000000003", "Another12345"); },
            "重复手机号注册失败");
    const Id checker = app.create_staff(
        admin, "checker", "checker", "13000000005", "Check12345");
    app.change_password(checker, "Check12345", "NewCheck12345");
    rejects([&] { app.register_user("admin", "evil", "13000000006", "Evil12345"); },
            "访客不能注册管理员");

    const Id category = app.add_category(admin, "音乐");
    check(app.categories().size() == 1 && app.categories()[0].id == category,
          "可列出当前可用活动类别");
    rejects([&] { app.add_category(admin, "音乐"); }, "类别名称唯一");
    const EventInput input = event_input(category, base + 2 * 86400, 1);
    const Id event = app.create_event(organizer, input);
    check(app.event(event).status == "draft", "草稿状态");
    app.submit_event(organizer, event);
    check(app.event(event).status == "under_review", "提交状态");
    app.review_event(admin, event, true, "-");
    check(app.event(event).status == "published", "发布状态");
    rejects([&] { app.edit_draft(organizer, event, input); }, "发布后不能改关键字段");
    app.edit_published_description(organizer, event, "发布后简介更新");

    SearchFilter filter;
    filter.category_id = category;
    filter.min_price_cents = 12800;
    filter.max_price_cents = 12800;
    filter.location = "南京";
    check(app.search_events(filter).size() == 1, "公开组合查询 AND 及区间边界");
    const Booking a = app.book(alice, event, "book-alice");
    check(a.order_status == "pending_payment", "报名待支付");
    check(app.book(alice, event, "book-alice").registration_id == a.registration_id,
          "相同请求幂等");
    check(app.event(event).occupied == 1, "最后一张被占位");
    rejects([&] { app.book(bob, event, "book-bob"); }, "售罄不能普通报名");
    const Booking b = app.join_waitlist(bob, event, "wait-bob");
    check(b.registration_status == "waitlisted", "候补状态");
    check(app.event(event).occupied == 1, "候补不占位");
    const Booking paid_a = app.pay(alice, a.registration_id, "pay-alice");
    check(!paid_a.ticket_code.empty(), "支付后出票");
    check(app.pay(alice, a.registration_id, "pay-again").ticket_code ==
              paid_a.ticket_code,
          "重复支付不会重复出票");
    check(app.booking(b.registration_id, bob).registration_status == "waitlisted",
          "A 支付后 B 留在候补");
    app.cancel_booking(alice, a.registration_id, "refund-alice");
    check(app.booking(a.registration_id, alice).order_status == "refunded",
          "退票转已退款");
    check(app.booking(b.registration_id, bob).order_status == "pending_payment",
          "释放后队首递补");
    check(app.event(event).occupied == 1, "名额转给 B 后占位仍为一");
    app.cancel_booking(alice, a.registration_id, "refund-repeat");
    check(app.event(event).occupied == 1, "重复退票不释放第二次");
    const Booking paid_b = app.pay(bob, b.registration_id, "pay-bob");
    check(!paid_b.ticket_code.empty(), "递补订单可支付");

    app.set_test_time(input.start_at);
    app.check_in(checker, event, paid_b.ticket_code, "scan-bob");
    rejects([&] { app.check_in(checker, event, paid_b.ticket_code, "scan-repeat"); },
            "重复签到拒绝");
    Stats stats = app.statistics(organizer, event);
    check(stats.sold == 1 && stats.checked_in == 1 &&
              stats.attendance_rate == 1.0 && stats.utilization_rate == 1.0,
          "统计口径与零退款票数");
    check(app.participant_list(organizer, event).size() == 1,
          "组织者查看本人活动名单");
    rejects([&] { app.participant_list(alice, event); },
            "参与者不得查看完整名单");
    check(app.risk_counts(admin, alice).voluntary_refunds == 1,
          "管理员核查主动退票次数");
    app.set_test_time(input.end_at);
    app.maintenance();
    check(app.booking(b.registration_id, bob).registration_status == "checked_in",
          "已签到不会误标缺席");
    check(!app.audit(admin, event).empty(), "审计记录可查询");
}

void timeout_and_report(const std::filesystem::path& file) {
    using namespace ticket;
    const Time base = parse_beijing_time("2026-11-13T10:00");
    App app(file);
    app.set_test_time(base);
    const Id admin = 1;
    const Id organizer = 2;
    const Id alice = 3;
    const Id bob = 4;
    const Id category = 1;
    const EventInput input = event_input(category, base + 3 * 86400, 1);
    const Id event = app.create_event(organizer, input);
    app.submit_event(organizer, event);
    app.review_event(admin, event, true, "-");
    const Booking a = app.book(alice, event, "timeout-a");
    const Booking b = app.join_waitlist(bob, event, "timeout-b");
    app.set_test_time(base + 15 * 60);
    app.maintenance();
    check(app.booking(a.registration_id, alice).order_status == "timed_out",
          "15 分钟边界转超时");
    check(app.booking(b.registration_id, bob).order_status == "pending_payment",
          "超时后递补");
    app.maintenance();
    check(app.event(event).occupied == 1, "重复定时任务不二次释放");
    rejects([&] { app.pay(alice, a.registration_id, "late-pay"); },
            "超时订单不得付款");

    const Id report = app.report_event(alice, event, "活动信息与说明不一致");
    rejects([&] { app.report_event(alice, event, "重复举报"); },
            "同人同活动只举报一次");
    app.decide_report(admin, report, true, "核查后确认属实");
    check(app.event(event).status == "removed", "举报属实且开始前下架");
    check(app.booking(b.registration_id, bob).order_status == "cancelled",
          "下架关闭待支付订单");
    check(app.event(event).occupied == 0, "下架释放名额");
    check(app.risk_counts(admin, organizer).organizer_violations == 1,
          "确认属实记一次组织者违规");
}

void concurrent_last_ticket(const std::filesystem::path& file) {
    using namespace ticket;
    const Time base = parse_beijing_time("2026-11-13T10:00");
    App setup(file);
    setup.set_test_time(base);
    const Id event = setup.create_event(2, event_input(1, base + 2 * 86400, 1));
    setup.submit_event(2, event);
    setup.review_event(1, event, true, "-");

    std::vector<Id> users;
    for (int index = 0; index < 20; ++index) {
        users.push_back(setup.register_user(
            "participant", "racer" + std::to_string(index),
            "14000000" + std::to_string(100 + index), "Racer12345"));
    }
    std::atomic<int> success{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 20; ++index) {
        threads.emplace_back([&, index] {
            try {
                App contender(file);
                contender.set_test_time(base);
                contender.book(users[index], event, "race-" + std::to_string(index));
                ++success;
            } catch (const BusinessError&) {
                // 其余参与者应收到售罄消息。
            } catch (const DatabaseError&) {
                // 锁超时也不能破坏库存；单独核对成功数量。
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(success == 1, "20 人争抢最后一张，恰好一人成功");
    check(setup.event(event).occupied == 1, "并发后库存仍为一");
}

}  // namespace

int main() {
    const std::filesystem::path file = "test_runtime.db";
    std::filesystem::remove(file);
    std::filesystem::remove("test_runtime.db-wal");
    std::filesystem::remove("test_runtime.db-shm");
    try {
        core_flow(file);
        timeout_and_report(file);
        concurrent_last_ticket(file);
        std::cout << "PASS: account, states, payment, waitlist, timeout, "
                     "refund, report, check-in, statistics, concurrency\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
