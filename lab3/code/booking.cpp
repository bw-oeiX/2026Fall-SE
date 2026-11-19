#include "app.hpp"

#include <openssl/rand.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace ticket {
namespace {

std::string make_ticket_code(Id order_id) {
    std::array<unsigned char, 8> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        throw BusinessError("无法生成唯一票码");
    }
    std::ostringstream result;
    result << "T-" << order_id << '-' << std::hex << std::setfill('0');
    for (unsigned char byte : random) {
        result << std::setw(2) << static_cast<int>(byte);
    }
    return result.str();
}

}  // namespace

Booking App::read_booking(Id registration_id) {
    auto query = db_.prepare(
        "SELECT r.id,r.event_id,r.participant_id,r.status,"
        "COALESCE(o.id,0),COALESCE(o.status,''),COALESCE(o.pay_deadline,0),"
        "COALESCE(t.code,'') "
        "FROM registrations r LEFT JOIN orders o ON o.registration_id=r.id "
        "LEFT JOIN tickets t ON t.order_id=o.id WHERE r.id=?");
    query.bind(1, registration_id);
    if (!query.step()) {
        throw BusinessError("报名记录不存在");
    }
    Booking result;
    result.registration_id = query.integer64(0);
    result.event_id = query.integer64(1);
    result.participant_id = query.integer64(2);
    result.registration_status = query.text(3);
    result.order_id = query.integer64(4);
    result.order_status = query.text(5);
    result.pay_deadline = query.integer64(6);
    result.ticket_code = query.text(7);
    return result;
}

Booking App::booking(Id registration_id, Id viewer_id) {
    maintenance();
    const Booking result = read_booking(registration_id);
    if (result.participant_id != viewer_id) {
        throw BusinessError("只能查看本人的报名和订单");
    }
    ensure_visible(viewer_id, event(result.event_id));
    return result;
}

std::vector<Booking> App::my_bookings(Id participant_id) {
    require_role(participant_id, "participant");
    maintenance();
    auto query = db_.prepare(
        "SELECT r.id FROM registrations r JOIN events e ON e.id=r.event_id "
        "WHERE r.participant_id=? AND e.end_at+15552000>? ORDER BY r.id DESC");
    query.bind(1, participant_id).bind(2, now());
    std::vector<Booking> result;
    while (query.step()) {
        result.push_back(read_booking(query.integer64(0)));
    }
    return result;
}

Booking App::book(Id participant_id, Id event_id, const std::string& request_id) {
    const User participant = require_role(participant_id, "participant");
    require_not_restricted(participant);
    if (request_id.empty()) {
        throw BusinessError("报名请求必须附带请求标识");
    }
    maintenance();
    Transaction transaction(db_);
    auto repeated = db_.prepare(
        "SELECT id,participant_id,event_id FROM registrations WHERE request_id=?");
    repeated.bind(1, request_id);
    if (repeated.step()) {
        if (repeated.integer64(1) != participant_id ||
            repeated.integer64(2) != event_id) {
            throw BusinessError("请求标识已被另一项报名使用");
        }
        const Id id = repeated.integer64(0);
        transaction.commit();
        return read_booking(id);
    }
    const Event target = event(event_id);
    if (target.status != "published" || now() < target.sale_start ||
        now() >= target.sale_end || now() >= target.start_at) {
        throw BusinessError("活动当前不可报名");
    }
    auto active = db_.prepare(
        "SELECT id FROM registrations WHERE event_id=? AND participant_id=? "
        "AND status IN ('waitlisted','pending_payment','purchased','checked_in')");
    active.bind(1, event_id).bind(2, participant_id);
    if (active.step()) {
        const Id existing_id = active.integer64(0);
        transaction.commit();
        return read_booking(existing_id);
    }
    auto occupy = db_.prepare(
        "UPDATE events SET occupied=occupied+1 WHERE id=? AND status='published' "
        "AND occupied<capacity");
    occupy.bind(1, event_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("名额已满，可选择加入候补");
    }
    auto insert = db_.prepare(
        "INSERT INTO registrations(event_id,participant_id,status,queue_seq,request_id,"
        "created_at,updated_at) VALUES(?,?,'pending_payment',0,?,?,?)");
    insert.bind(1, event_id).bind(2, participant_id).bind(3, request_id)
        .bind(4, now()).bind(5, now()).execute();
    const Id registration_id = db_.last_id();
    auto order = db_.prepare(
        "INSERT INTO orders(registration_id,amount_cents,status,pay_deadline,"
        "created_at,updated_at) VALUES(?,?,'pending_payment',?,?,?)");
    order.bind(1, registration_id).bind(2, target.price_cents)
        .bind(3, now() + 15 * 60).bind(4, now()).bind(5, now()).execute();
    audit_write(participant_id, event_id, "registration", registration_id,
                "book", "", "pending_payment", "success", "", request_id);
    transaction.commit();
    return read_booking(registration_id);
}

Booking App::join_waitlist(Id participant_id, Id event_id,
                           const std::string& request_id) {
    const User participant = require_role(participant_id, "participant");
    require_not_restricted(participant);
    if (request_id.empty()) {
        throw BusinessError("候补请求必须附带请求标识");
    }
    maintenance();
    Transaction transaction(db_);
    auto repeated = db_.prepare(
        "SELECT id,participant_id,event_id FROM registrations WHERE request_id=?");
    repeated.bind(1, request_id);
    if (repeated.step()) {
        if (repeated.integer64(1) != participant_id ||
            repeated.integer64(2) != event_id) {
            throw BusinessError("请求标识已被另一项报名使用");
        }
        const Id id = repeated.integer64(0);
        transaction.commit();
        return read_booking(id);
    }
    const Event target = event(event_id);
    if (target.status != "published" || now() < target.sale_start ||
        now() >= target.sale_end || now() >= target.start_at ||
        target.occupied < target.capacity) {
        throw BusinessError("仅在放票期售罄时可加入候补");
    }
    auto active = db_.prepare(
        "SELECT id FROM registrations WHERE event_id=? AND participant_id=? "
        "AND status IN ('waitlisted','pending_payment','purchased','checked_in')");
    active.bind(1, event_id).bind(2, participant_id);
    if (active.step()) {
        const Id id = active.integer64(0);
        transaction.commit();
        return read_booking(id);
    }
    auto overlap = db_.prepare(
        "SELECT COUNT(*) FROM registrations r JOIN events e ON e.id=r.event_id "
        "WHERE r.participant_id=? AND r.status='waitlisted' "
        "AND e.start_at<? AND e.end_at>?");
    overlap.bind(1, participant_id).bind(2, target.end_at).bind(3, target.start_at);
    overlap.step();
    if (overlap.integer(0) > 0) {
        throw BusinessError("不能同时候补时间重叠的活动");
    }
    auto sequence = db_.prepare(
        "SELECT COALESCE(MAX(queue_seq),0)+1 FROM registrations WHERE event_id=?");
    sequence.bind(1, event_id);
    sequence.step();
    const Id queue_number = sequence.integer64(0);
    auto insert = db_.prepare(
        "INSERT INTO registrations(event_id,participant_id,status,queue_seq,request_id,"
        "created_at,updated_at) VALUES(?,?,'waitlisted',?,?,?,?)");
    insert.bind(1, event_id).bind(2, participant_id).bind(3, queue_number)
        .bind(4, request_id).bind(5, now()).bind(6, now()).execute();
    const Id id = db_.last_id();
    audit_write(participant_id, event_id, "registration", id,
                "join_waitlist", "", "waitlisted", "success", "", request_id);
    transaction.commit();
    return read_booking(id);
}

Booking App::pay(Id participant_id, Id registration_id,
                 const std::string& request_id) {
    require_role(participant_id, "participant");
    maintenance();
    Transaction transaction(db_);
    const Booking current = read_booking(registration_id);
    if (current.participant_id != participant_id) {
        throw BusinessError("不能支付他人的订单");
    }
    if (current.order_status == "paid") {
        transaction.commit();
        return current;
    }
    if (current.order_status != "pending_payment" ||
        current.pay_deadline <= now() || event(current.event_id).status != "published") {
        throw BusinessError("订单不是可支付状态或已到期");
    }
    auto order = db_.prepare(
        "UPDATE orders SET status='paid',updated_at=? WHERE id=? "
        "AND status='pending_payment' AND pay_deadline>?");
    order.bind(1, now()).bind(2, current.order_id).bind(3, now()).execute();
    if (db_.changes() != 1) {
        throw BusinessError("订单状态已改变，请重新加载");
    }
    auto registration = db_.prepare(
        "UPDATE registrations SET status='purchased',updated_at=? WHERE id=? "
        "AND status='pending_payment'");
    registration.bind(1, now()).bind(2, registration_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("报名状态已改变，请重新加载");
    }
    auto ticket = db_.prepare(
        "INSERT INTO tickets(order_id,code,status) VALUES(?,?,'valid')");
    ticket.bind(1, current.order_id)
        .bind(2, make_ticket_code(current.order_id)).execute();
    audit_write(participant_id, current.event_id, "order", current.order_id,
                "pay", "pending_payment", "paid", "success", "", request_id);
    notice(participant_id, current.event_id, "模拟支付成功，电子票已生成");
    transaction.commit();
    return read_booking(registration_id);
}

void App::promote_waitlist(Id event_id) {
    const Event target = event(event_id);
    if (target.status != "published" || now() < target.sale_start ||
        now() >= target.sale_end || target.start_at - now() < 12 * 3600 ||
        target.occupied >= target.capacity) {
        return;
    }
    auto next = db_.prepare(
        "SELECT id,participant_id FROM registrations WHERE event_id=? "
        "AND status='waitlisted' ORDER BY queue_seq,id LIMIT 1");
    next.bind(1, event_id);
    if (!next.step()) {
        return;
    }
    const Id registration_id = next.integer64(0);
    const Id participant_id = next.integer64(1);
    auto occupy = db_.prepare(
        "UPDATE events SET occupied=occupied+1 WHERE id=? AND occupied<capacity");
    occupy.bind(1, event_id).execute();
    if (db_.changes() != 1) {
        return;
    }
    auto update = db_.prepare(
        "UPDATE registrations SET status='pending_payment',updated_at=? "
        "WHERE id=? AND status='waitlisted'");
    update.bind(1, now()).bind(2, registration_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("候补状态冲突");
    }
    auto order = db_.prepare(
        "INSERT INTO orders(registration_id,amount_cents,status,pay_deadline,"
        "created_at,updated_at) VALUES(?,?,'pending_payment',?,?,?)");
    order.bind(1, registration_id).bind(2, target.price_cents)
        .bind(3, now() + 15 * 60).bind(4, now()).bind(5, now()).execute();
    audit_write(0, event_id, "registration", registration_id,
                "promote_waitlist", "waitlisted", "pending_payment");
    notice(participant_id, event_id, "候补递补成功，请在 15 分钟内确认模拟支付");
}

void App::release_slot(Id event_id) {
    auto update = db_.prepare(
        "UPDATE events SET occupied=occupied-1 WHERE id=? AND occupied>0");
    update.bind(1, event_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("库存不一致：没有可释放的占位");
    }
    promote_waitlist(event_id);
}

void App::cancel_booking(Id participant_id, Id registration_id,
                         const std::string& request_id) {
    require_role(participant_id, "participant");
    maintenance();
    Transaction transaction(db_);
    const Booking current = read_booking(registration_id);
    if (current.participant_id != participant_id) {
        throw BusinessError("不能取消他人的报名");
    }
    if (current.registration_status == "cancelled" ||
        current.registration_status == "invalid") {
        transaction.commit();
        return;
    }
    const Event target = event(current.event_id);
    if (now() < target.sale_start || now() >= target.sale_end ||
        now() >= target.start_at) {
        throw BusinessError("仅在放票期、活动开始前可取消或退票");
    }
    if (current.registration_status == "checked_in" ||
        current.registration_status == "absent") {
        throw BusinessError("已签到或已结束的票不能退");
    }
    auto registration = db_.prepare(
        "UPDATE registrations SET status='cancelled',updated_at=? WHERE id=?");
    registration.bind(1, now()).bind(2, registration_id).execute();
    if (current.order_id != 0) {
        const std::string final_order = current.order_status == "paid"
                                            ? "refunded" : "cancelled";
        auto order = db_.prepare(
            "UPDATE orders SET status=?,updated_at=? WHERE id=?");
        order.bind(1, final_order).bind(2, now())
            .bind(3, current.order_id).execute();
        if (current.order_status == "paid") {
            auto ticket = db_.prepare(
                "UPDATE tickets SET status='invalid' WHERE order_id=? AND status='valid'");
            ticket.bind(1, current.order_id).execute();
        }
        release_slot(current.event_id);
    }
    audit_write(participant_id, current.event_id, "registration", registration_id,
                "cancel_booking", current.registration_status, "cancelled",
                "success", "", request_id);
    notice(participant_id, current.event_id,
           current.order_status == "paid" ? "退票完成，已模拟退款" : "报名已取消");
    transaction.commit();
}

void App::check_in(Id checker_id, Id event_id,
                   const std::string& ticket_code,
                   const std::string& request_id) {
    require_role(checker_id, "checker");
    const Event target = event(event_id);
    if (target.status != "published" || now() < target.start_at ||
        now() >= target.end_at) {
        throw BusinessError("不在活动有效检票时间内");
    }
    Transaction transaction(db_);
    auto query = db_.prepare(
        "SELECT t.id,t.status,r.id,r.status,o.status,e.id "
        "FROM tickets t JOIN orders o ON o.id=t.order_id "
        "JOIN registrations r ON r.id=o.registration_id "
        "JOIN events e ON e.id=r.event_id WHERE t.code=?");
    query.bind(1, ticket_code);
    if (!query.step() || query.integer64(5) != event_id ||
        query.text(1) != "valid" || query.text(3) != "purchased" ||
        query.text(4) != "paid") {
        throw BusinessError("票码无效、已验票或不属于所选活动");
    }
    const Id ticket_id = query.integer64(0);
    const Id registration_id = query.integer64(2);
    auto ticket = db_.prepare(
        "UPDATE tickets SET status='checked',checked_at=? "
        "WHERE id=? AND status='valid'");
    ticket.bind(1, now()).bind(2, ticket_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("重复验票");
    }
    auto registration = db_.prepare(
        "UPDATE registrations SET status='checked_in',updated_at=? "
        "WHERE id=? AND status='purchased'");
    registration.bind(1, now()).bind(2, registration_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("报名状态不允许签到");
    }
    audit_write(checker_id, event_id, "ticket", ticket_id, "check_in",
                "valid", "checked", "success", "", request_id);
    transaction.commit();
}

void App::maintenance() {
    Transaction transaction(db_);
    auto expired = db_.prepare(
        "SELECT o.id,r.id,r.event_id FROM orders o JOIN registrations r "
        "ON r.id=o.registration_id WHERE o.status='pending_payment' "
        "AND o.pay_deadline<=? ORDER BY o.pay_deadline,o.id");
    expired.bind(1, now());
    struct Expired { Id order; Id registration; Id event; };
    std::vector<Expired> rows;
    while (expired.step()) {
        rows.push_back({expired.integer64(0), expired.integer64(1),
                        expired.integer64(2)});
    }
    for (const auto& row : rows) {
        auto order = db_.prepare(
            "UPDATE orders SET status='timed_out',updated_at=? "
            "WHERE id=? AND status='pending_payment'");
        order.bind(1, now()).bind(2, row.order).execute();
        if (db_.changes() != 1) continue;
        auto registration = db_.prepare(
            "UPDATE registrations SET status='invalid',updated_at=? "
            "WHERE id=? AND status='pending_payment'");
        registration.bind(1, now()).bind(2, row.registration).execute();
        release_slot(row.event);
        audit_write(0, row.event, "order", row.order, "payment_timeout",
                    "pending_payment", "timed_out");
    }
    auto absent = db_.prepare(
        "SELECT r.id,r.event_id FROM registrations r JOIN events e "
        "ON e.id=r.event_id WHERE r.status='purchased' AND e.end_at<=?");
    absent.bind(1, now());
    std::vector<std::pair<Id, Id>> missed;
    while (absent.step()) {
        missed.emplace_back(absent.integer64(0), absent.integer64(1));
    }
    for (const auto& [registration_id, event_id] : missed) {
        auto update = db_.prepare(
            "UPDATE registrations SET status='absent',updated_at=? "
            "WHERE id=? AND status='purchased'");
        update.bind(1, now()).bind(2, registration_id).execute();
        audit_write(0, event_id, "registration", registration_id,
                    "mark_absent", "purchased", "absent");
    }
    transaction.commit();
}

}  // namespace ticket
