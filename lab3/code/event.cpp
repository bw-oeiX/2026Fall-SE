#include "app.hpp"

#include <algorithm>

namespace ticket {

Event App::read_event(Statement& query) {
    Event result;
    result.id = query.integer64(0);
    result.organizer_id = query.integer64(1);
    result.category_id = query.integer64(2);
    result.name = query.text(3);
    result.description = query.text(4);
    result.location = query.text(5);
    result.start_at = query.integer64(6);
    result.end_at = query.integer64(7);
    result.sale_start = query.integer64(8);
    result.sale_end = query.integer64(9);
    result.price_cents = query.integer(10);
    result.capacity = query.integer(11);
    result.occupied = query.integer(12);
    result.status = query.text(13);
    result.review_reason = query.text(14);
    return result;
}

Event App::event(Id event_id) {
    auto query = db_.prepare(
        "SELECT id,organizer_id,category_id,name,description,location,start_at,"
        "end_at,sale_start,sale_end,price_cents,capacity,occupied,status,review_reason "
        "FROM events WHERE id=?");
    query.bind(1, event_id);
    if (!query.step()) {
        throw BusinessError("活动不存在");
    }
    return read_event(query);
}

void App::validate_event(const EventInput& input) {
    if (input.name.empty() || utf8_characters(input.name) > 100) {
        throw BusinessError("活动名称不能为空且不得超过 100 字");
    }
    if (utf8_characters(input.description) > 500) {
        throw BusinessError("活动简介不得超过 500 字");
    }
    if (input.location.empty() || utf8_characters(input.location) > 60) {
        throw BusinessError("地点不能为空且不得超过 60 字");
    }
    if (input.price_cents < 0 || input.price_cents > 99999900) {
        throw BusinessError("票价超出 0～999999 元范围");
    }
    if (input.capacity < 1 || input.capacity > 9999) {
        throw BusinessError("容量须为 1～9999");
    }
    if (input.sale_start >= input.sale_end ||
        input.sale_end > input.start_at ||
        input.start_at >= input.end_at) {
        throw BusinessError("放票和场次时间顺序不合法");
    }
    auto category = db_.prepare("SELECT enabled FROM categories WHERE id=?");
    category.bind(1, input.category_id);
    if (!category.step() || category.integer(0) == 0) {
        throw BusinessError("活动类别不存在或已停用");
    }
}

Id App::add_category(Id admin_id, const std::string& name) {
    require_role(admin_id, "admin");
    if (name.empty() || utf8_characters(name) > 40) {
        throw BusinessError("类别名称不能为空且不得超过 40 字");
    }
    Transaction transaction(db_);
    auto insert = db_.prepare("INSERT INTO categories(name) VALUES(?)");
    insert.bind(1, name).execute();
    const Id id = db_.last_id();
    audit_write(admin_id, 0, "category", id, "add_category", "", name);
    transaction.commit();
    return id;
}

void App::disable_category(Id admin_id, Id category_id) {
    require_role(admin_id, "admin");
    Transaction transaction(db_);
    auto update = db_.prepare("UPDATE categories SET enabled=0 WHERE id=? AND enabled=1");
    update.bind(1, category_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("类别不存在或已经停用");
    }
    audit_write(admin_id, 0, "category", category_id, "disable_category", "enabled",
                "disabled");
    transaction.commit();
}

std::vector<Category> App::categories() {
    auto query = db_.prepare("SELECT id,name FROM categories WHERE enabled=1 ORDER BY id");
    std::vector<Category> result;
    while (query.step()) {
        result.push_back({query.integer64(0), query.text(1)});
    }
    return result;
}

Id App::create_event(Id organizer_id, const EventInput& input) {
    require_role(organizer_id, "organizer");
    validate_event(input);
    Transaction transaction(db_);
    auto insert = db_.prepare(
        "INSERT INTO events(organizer_id,category_id,name,description,location,"
        "start_at,end_at,sale_start,sale_end,price_cents,capacity,status,created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,'draft',?,?)");
    insert.bind(1, organizer_id).bind(2, input.category_id)
        .bind(3, input.name).bind(4, input.description).bind(5, input.location)
        .bind(6, input.start_at).bind(7, input.end_at)
        .bind(8, input.sale_start).bind(9, input.sale_end)
        .bind(10, input.price_cents).bind(11, input.capacity)
        .bind(12, now()).bind(13, now()).execute();
    const Id id = db_.last_id();
    audit_write(organizer_id, id, "event", id, "create_draft", "", "draft");
    transaction.commit();
    return id;
}

void App::edit_draft(Id organizer_id, Id event_id, const EventInput& input) {
    require_role(organizer_id, "organizer");
    const Event before = event(event_id);
    if (before.organizer_id != organizer_id || before.status != "draft") {
        throw BusinessError("只能修改本人处于草稿状态的活动");
    }
    validate_event(input);
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE events SET category_id=?,name=?,description=?,location=?,"
        "start_at=?,end_at=?,sale_start=?,sale_end=?,price_cents=?,capacity=?,updated_at=? "
        "WHERE id=? AND organizer_id=? AND status='draft'");
    update.bind(1, input.category_id).bind(2, input.name)
        .bind(3, input.description).bind(4, input.location)
        .bind(5, input.start_at).bind(6, input.end_at)
        .bind(7, input.sale_start).bind(8, input.sale_end)
        .bind(9, input.price_cents).bind(10, input.capacity)
        .bind(11, now()).bind(12, event_id).bind(13, organizer_id).execute();
    if (db_.changes() != 1) {
        throw BusinessError("草稿已发生变化，请重新加载");
    }
    audit_write(organizer_id, event_id, "event", event_id, "edit_draft",
                before.name, input.name);
    transaction.commit();
}

void App::edit_published_description(Id organizer_id, Id event_id,
                                     const std::string& description) {
    require_role(organizer_id, "organizer");
    const Event before = event(event_id);
    if (before.organizer_id != organizer_id || before.status != "published") {
        throw BusinessError("只能修改本人已发布活动的简介");
    }
    if (utf8_characters(description) > 500) {
        throw BusinessError("简介不得超过 500 字");
    }
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE events SET description=?,updated_at=? WHERE id=? AND status='published'");
    update.bind(1, description).bind(2, now()).bind(3, event_id).execute();
    audit_write(organizer_id, event_id, "event", event_id,
                "edit_published_description", before.description, description);
    transaction.commit();
}

void App::submit_event(Id organizer_id, Id event_id) {
    const User organizer = require_role(organizer_id, "organizer");
    require_not_restricted(organizer);
    const Event current = event(event_id);
    if (current.organizer_id != organizer_id || current.status != "draft") {
        throw BusinessError("仅可提交本人的草稿活动");
    }
    if (current.start_at < now() || current.start_at > now() + 7 * 86400) {
        throw BusinessError("提交时活动开始应在当前至 7 天内");
    }
    if (current.sale_end <= now()) {
        throw BusinessError("放票已经结束，不能提交审核");
    }
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE events SET status='under_review',updated_at=? "
        "WHERE id=? AND status='draft'");
    update.bind(1, now()).bind(2, event_id).execute();
    audit_write(organizer_id, event_id, "event", event_id, "submit_review",
                "draft", "under_review");
    transaction.commit();
}

void App::withdraw_event(Id organizer_id, Id event_id) {
    require_role(organizer_id, "organizer");
    const Event current = event(event_id);
    if (current.organizer_id != organizer_id || current.status != "under_review") {
        throw BusinessError("仅可撤回本人审核中的活动");
    }
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE events SET status='draft',updated_at=? WHERE id=? AND status='under_review'");
    update.bind(1, now()).bind(2, event_id).execute();
    audit_write(organizer_id, event_id, "event", event_id, "withdraw_review",
                "under_review", "draft");
    transaction.commit();
}

void App::review_event(Id admin_id, Id event_id, bool approved,
                       const std::string& reason) {
    require_role(admin_id, "admin");
    const Event current = event(event_id);
    if (current.status != "under_review") {
        throw BusinessError("只有审核中的活动可处理");
    }
    if (!approved && reason.empty()) {
        throw BusinessError("驳回活动必须填写原因");
    }
    if (approved && current.sale_end <= now()) {
        throw BusinessError("放票已经结束，须驳回并要求修改");
    }
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE events SET status=?,review_reason=?,updated_at=? "
        "WHERE id=? AND status='under_review'");
    const std::string after = approved ? "published" : "draft";
    update.bind(1, after).bind(2, reason).bind(3, now())
        .bind(4, event_id).execute();
    audit_write(admin_id, event_id, "event", event_id,
                approved ? "approve" : "reject", "under_review", after,
                "success", reason);
    notice(current.organizer_id, event_id,
           approved ? "活动审核通过并发布" : "活动审核驳回：" + reason);
    transaction.commit();
}

void App::close_event_records(Id event_id, const std::string& final_status) {
    auto active = db_.prepare(
        "SELECT r.id,r.participant_id,r.status,o.id,o.status,t.id "
        "FROM registrations r LEFT JOIN orders o ON o.registration_id=r.id "
        "LEFT JOIN tickets t ON t.order_id=o.id "
        "WHERE r.event_id=? AND r.status IN "
        "('waitlisted','pending_payment','purchased')");
    active.bind(1, event_id);
    struct Row { Id registration; Id participant; std::string status; Id order; 
                 std::string order_status; Id ticket; };
    std::vector<Row> rows;
    while (active.step()) {
        rows.push_back({active.integer64(0), active.integer64(1), active.text(2),
                        active.is_null(3) ? 0 : active.integer64(3),
                        active.text(4), active.is_null(5) ? 0 : active.integer64(5)});
    }
    for (const Row& row : rows) {
        const std::string registration_end = row.status == "purchased"
                                                 ? "invalid" : "cancelled";
        auto registration = db_.prepare(
            "UPDATE registrations SET status=?,updated_at=? WHERE id=?");
        registration.bind(1, registration_end).bind(2, now())
            .bind(3, row.registration).execute();
        if (row.order != 0) {
            const std::string order_end = row.order_status == "paid"
                                              ? "refunded" : "cancelled";
            auto order = db_.prepare(
                "UPDATE orders SET status=?,updated_at=? WHERE id=?");
            order.bind(1, order_end).bind(2, now())
                .bind(3, row.order).execute();
        }
        if (row.ticket != 0) {
            auto ticket = db_.prepare(
                "UPDATE tickets SET status='invalid' WHERE id=? AND status='valid'");
            ticket.bind(1, row.ticket).execute();
        }
        notice(row.participant, event_id,
               "活动" + final_status + "；待支付和候补已关闭，已支付订单已模拟退款");
    }
    auto reset = db_.prepare("UPDATE events SET occupied=0 WHERE id=?");
    reset.bind(1, event_id).execute();
}

void App::cancel_event(Id organizer_id, Id event_id) {
    require_role(organizer_id, "organizer");
    const Event current = event(event_id);
    if (current.organizer_id != organizer_id || current.status != "published" ||
        now() >= current.start_at) {
        throw BusinessError("仅可在开始前取消本人已发布的活动");
    }
    Transaction transaction(db_);
    auto update = db_.prepare(
        "UPDATE events SET status='cancelled',updated_at=? WHERE id=? "
        "AND status='published'");
    update.bind(1, now()).bind(2, event_id).execute();
    close_event_records(event_id, "已取消");
    audit_write(organizer_id, event_id, "event", event_id, "cancel_event",
                "published", "cancelled", "success", "组织者主动取消，记违规一次");
    transaction.commit();
}

void App::ensure_visible(Id viewer_id, const Event& value) {
    if (value.end_at + 180 * 86400 <= now()) {
        throw BusinessError("活动普通业务数据已归档");
    }
    const User viewer = user(viewer_id);
    if (viewer.role == "organizer" && value.organizer_id != viewer_id) {
        throw BusinessError("不能访问其他组织者的活动数据");
    }
}

std::vector<Event> App::search_events(const SearchFilter& filter,
                                      std::optional<Id> viewer) {
    if (filter.page < 1 || filter.page_size < 1) {
        throw BusinessError("页码和页大小必须为正整数");
    }
    const int page_size = std::min(filter.page_size, 100);
    std::string role;
    if (viewer) {
        role = user(*viewer).role;
    }
    std::string sql =
        "SELECT id,organizer_id,category_id,name,description,location,start_at,"
        "end_at,sale_start,sale_end,price_cents,capacity,occupied,status,review_reason "
        "FROM events WHERE 1=1";
    if (role.empty() || role == "participant" || role == "checker") {
        sql += " AND status='published'";
    } else if (role == "organizer") {
        sql += " AND (status='published' OR organizer_id=" + std::to_string(*viewer) + ")";
    }
    if (filter.category_id) sql += " AND category_id=" + std::to_string(*filter.category_id);
    if (filter.min_price_cents) sql += " AND price_cents>=" + std::to_string(*filter.min_price_cents);
    if (filter.max_price_cents) sql += " AND price_cents<=" + std::to_string(*filter.max_price_cents);
    if (filter.from_time) sql += " AND start_at>=" + std::to_string(*filter.from_time);
    if (filter.to_time) sql += " AND start_at<=" + std::to_string(*filter.to_time);
    if (!filter.location.empty()) sql += " AND location LIKE ?";
    if (!filter.status.empty()) {
        if (filter.status != "draft" && filter.status != "under_review" &&
            filter.status != "published" && filter.status != "cancelled" &&
            filter.status != "removed") {
            throw BusinessError("未知活动状态");
        }
        sql += " AND status='" + filter.status + "'";
    }
    sql += filter.order_by == "remaining"
               ? " ORDER BY (capacity-occupied) DESC,id ASC"
               : " ORDER BY start_at ASC,id ASC";
    sql += " LIMIT " + std::to_string(page_size) +
           " OFFSET " + std::to_string((filter.page - 1) * page_size);
    auto query = db_.prepare(sql);
    if (!filter.location.empty()) query.bind(1, "%" + filter.location + "%");
    std::vector<Event> result;
    while (query.step()) {
        result.push_back(read_event(query));
    }
    return result;
}

}  // namespace ticket
