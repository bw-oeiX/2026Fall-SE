#include "app.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace ticket {

Id App::report_event(Id participant_id, Id event_id,
                     const std::string& reason) {
    require_role(participant_id, "participant");
    if (reason.empty() || utf8_characters(reason) > 500) {
        throw BusinessError("举报原因不能为空且不得超过 500 字");
    }
    const Event target = event(event_id);
    if (target.status != "published") {
        throw BusinessError("仅可举报已发布活动");
    }
    Transaction transaction(db_);
    auto repeated = db_.prepare(
        "SELECT COUNT(*) FROM reports WHERE reporter_id=? AND event_id=?");
    repeated.bind(1, participant_id).bind(2, event_id);
    repeated.step();
    if (repeated.integer(0) != 0) {
        throw BusinessError("同一活动只能举报一次");
    }
    auto count = db_.prepare(
        "SELECT COUNT(*) FROM reports WHERE reporter_id=? AND created_at>=?");
    count.bind(1, participant_id).bind(2, now() - 30 * 86400);
    count.step();
    if (count.integer(0) >= 5) {
        throw BusinessError("连续 30 天最多举报 5 次");
    }
    auto insert = db_.prepare(
        "INSERT INTO reports(event_id,reporter_id,reason,status,created_at) "
        "VALUES(?,?,?,'pending',?)");
    insert.bind(1, event_id).bind(2, participant_id)
        .bind(3, reason).bind(4, now()).execute();
    const Id id = db_.last_id();
    audit_write(participant_id, event_id, "report", id, "report_event",
                "", "pending", "success", reason);
    transaction.commit();
    return id;
}

void App::decide_report(Id admin_id, Id report_id, bool confirmed,
                        const std::string& reason) {
    require_role(admin_id, "admin");
    if (reason.empty()) {
        throw BusinessError("举报核查必须填写结论原因");
    }
    Transaction transaction(db_);
    auto report = db_.prepare(
        "SELECT event_id,status FROM reports WHERE id=?");
    report.bind(1, report_id);
    if (!report.step() || report.text(1) != "pending") {
        throw BusinessError("举报不存在或已处理");
    }
    const Id event_id = report.integer64(0);
    const Event target = event(event_id);
    const std::string decision = confirmed ? "true" : "false";
    auto update = db_.prepare(
        "UPDATE reports SET status=?,result_reason=?,decided_at=? "
        "WHERE id=? AND status='pending'");
    update.bind(1, decision).bind(2, reason)
        .bind(3, now()).bind(4, report_id).execute();
    if (confirmed && target.status == "published" && now() < target.start_at) {
        auto remove = db_.prepare(
            "UPDATE events SET status='removed',updated_at=? WHERE id=? "
            "AND status='published'");
        remove.bind(1, now()).bind(2, event_id).execute();
        close_event_records(event_id, "已下架");
        notice(target.organizer_id, event_id, "举报核查属实，活动已下架");
        audit_write(admin_id, event_id, "event", event_id, "remove_event",
                    "published", "removed", "success", reason);
    } else if (confirmed) {
        notice(target.organizer_id, event_id,
               "举报核查属实；活动已开始，不作下架或已签到状态修改");
    }
    audit_write(admin_id, event_id, "report", report_id, "decide_report",
                "pending", decision, "success", reason);
    transaction.commit();
}

Stats App::statistics(Id viewer_id, Id event_id) {
    maintenance();
    const Event target = event(event_id);
    ensure_visible(viewer_id, target);
    const User viewer = user(viewer_id);
    if (viewer.role == "participant") {
        throw BusinessError("参与者无权查看活动汇总统计");
    }
    Stats result;
    result.event_id = event_id;
    result.capacity = target.capacity;
    result.remaining = target.capacity - target.occupied;
    auto counts = db_.prepare(
        "SELECT "
        "SUM(CASE WHEN r.status='waitlisted' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN o.status='pending_payment' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN o.status='paid' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN r.status='checked_in' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN r.status='absent' THEN 1 ELSE 0 END) "
        "FROM registrations r LEFT JOIN orders o ON o.registration_id=r.id "
        "WHERE r.event_id=?");
    counts.bind(1, event_id);
    counts.step();
    result.waitlisted = counts.integer(0);
    result.pending_payment = counts.integer(1);
    result.sold = counts.integer(2);
    result.checked_in = counts.integer(3);
    result.absent = counts.integer(4);
    if (result.sold > 0) {
        result.attendance_rate = static_cast<double>(result.checked_in) / result.sold;
        result.absence_rate = 1.0 - result.attendance_rate;
    }
    if (result.capacity > 0) {
        result.utilization_rate = static_cast<double>(result.checked_in) /
                                  result.capacity;
    }
    if (result.remaining != result.capacity - result.pending_payment - result.sold) {
        throw BusinessError("库存与订单统计不一致");
    }
    return result;
}

std::vector<ParticipantRow> App::participant_list(Id viewer_id, Id event_id) {
    const User viewer = user(viewer_id);
    const Event target = event(event_id);
    ensure_visible(viewer_id, target);
    if (viewer.role != "organizer" && viewer.role != "checker" &&
        viewer.role != "admin") {
        throw BusinessError("当前角色无权查看活动参与者名单");
    }
    auto query = db_.prepare(
        "SELECT u.username,COALESCE(t.code,''),r.status "
        "FROM registrations r JOIN users u ON u.id=r.participant_id "
        "LEFT JOIN orders o ON o.registration_id=r.id "
        "LEFT JOIN tickets t ON t.order_id=o.id "
        "WHERE r.event_id=? AND o.status='paid' ORDER BY r.id");
    query.bind(1, event_id);
    std::vector<ParticipantRow> result;
    while (query.step()) {
        result.push_back({query.text(0), query.text(1), query.text(2)});
    }
    return result;
}

RiskCounts App::risk_counts(Id admin_id, Id target_id) {
    require_role(admin_id, "admin");
    const User target = user(target_id);
    const Time from = now() - 30 * 86400;
    RiskCounts result;
    if (target.role == "participant") {
        auto absent = db_.prepare(
            "SELECT COUNT(*) FROM registrations WHERE participant_id=? "
            "AND status='absent' AND updated_at>=?");
        absent.bind(1, target_id).bind(2, from);
        absent.step();
        result.absent = absent.integer(0);
        auto refunds = db_.prepare(
            "SELECT COUNT(*) FROM audit_logs WHERE actor_id=? "
            "AND action='cancel_booking' AND before_value='purchased' "
            "AND created_at>=?");
        refunds.bind(1, target_id).bind(2, from);
        refunds.step();
        result.voluntary_refunds = refunds.integer(0);
        result.pending_admin_review = result.absent >= 3 ||
                                      result.voluntary_refunds >= 5;
    } else if (target.role == "organizer") {
        auto violations = db_.prepare(
            "SELECT COUNT(*) FROM audit_logs a JOIN events e ON e.id=a.event_id "
            "WHERE e.organizer_id=? AND a.created_at>=? AND "
            "(a.action='cancel_event' OR "
            "(a.action='decide_report' AND a.after_value='true'))");
        violations.bind(1, target_id).bind(2, from);
        violations.step();
        result.organizer_violations = violations.integer(0);
        result.pending_admin_review = result.organizer_violations >= 3;
    } else {
        throw BusinessError("仅参与者和组织者有违规阈值统计");
    }
    return result;
}

std::vector<AuditEntry> App::audit(Id admin_id, Id event_id, int limit) {
    require_role(admin_id, "admin");
    if (limit < 1 || limit > 1000) {
        throw BusinessError("审计查询条数须在 1～1000 之间");
    }
    auto query = db_.prepare(
        "SELECT id,COALESCE(actor_id,0),COALESCE(event_id,0),object_type,"
        "COALESCE(object_id,0),action,before_value,after_value,result,reason,"
        "request_id,created_at FROM audit_logs WHERE (?=0 OR event_id=?) "
        "ORDER BY id DESC LIMIT ?");
    query.bind(1, event_id).bind(2, event_id).bind(3, limit);
    std::vector<AuditEntry> entries;
    while (query.step()) {
        AuditEntry entry;
        entry.id = query.integer64(0);
        entry.actor_id = query.integer64(1);
        entry.event_id = query.integer64(2);
        entry.object_type = query.text(3);
        entry.object_id = query.integer64(4);
        entry.action = query.text(5);
        entry.before_value = query.text(6);
        entry.after_value = query.text(7);
        entry.result = query.text(8);
        entry.reason = query.text(9);
        entry.request_id = query.text(10);
        entry.created_at = query.integer64(11);
        entries.push_back(std::move(entry));
    }
    return entries;
}

void App::export_csv(Id viewer_id, Id event_id,
                     const std::filesystem::path& file,
                     const std::string& kind) {
    const User viewer = user(viewer_id);
    const Event target = event(event_id);
    ensure_visible(viewer_id, target);
    if (kind == "statistics") {
        if (viewer.role != "organizer" && viewer.role != "admin") {
            throw BusinessError("仅组织者或管理员可导出统计");
        }
    } else if (kind == "list") {
        if (viewer.role != "checker" && viewer.role != "admin") {
            throw BusinessError("仅检票人员或管理员可导出名单");
        }
    } else if (kind == "audit") {
        if (viewer.role != "admin") {
            throw BusinessError("仅管理员可导出审计记录");
        }
    } else {
        throw BusinessError("未知导出类型");
    }
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path());
    }
    std::ofstream output(file, std::ios::binary);
    if (!output) {
        throw BusinessError("无法创建 CSV 文件");
    }
    output.write("\xEF\xBB\xBF", 3);
    if (kind == "statistics") {
        const Stats s = statistics(viewer_id, event_id);
        output << "event_id,capacity,remaining,pending,sold,waitlisted,checked_in,"
                  "absent,attendance_rate,absence_rate,utilization_rate\r\n";
        output << s.event_id << ',' << s.capacity << ',' << s.remaining << ','
               << s.pending_payment << ',' << s.sold << ',' << s.waitlisted << ','
               << s.checked_in << ',' << s.absent << ',' << s.attendance_rate << ','
               << s.absence_rate << ',' << s.utilization_rate << "\r\n";
    } else if (kind == "list") {
        output << "participant_name,ticket_code,check_in_status\r\n";
        auto query = db_.prepare(
            "SELECT u.username,COALESCE(t.code,''),r.status "
            "FROM registrations r JOIN users u ON u.id=r.participant_id "
            "LEFT JOIN orders o ON o.registration_id=r.id "
            "LEFT JOIN tickets t ON t.order_id=o.id "
            "WHERE r.event_id=? AND o.status='paid' ORDER BY r.id");
        query.bind(1, event_id);
        while (query.step()) {
            output << csv_escape(query.text(0)) << ','
                   << csv_escape(query.text(1)) << ','
                   << csv_escape(query.text(2)) << "\r\n";
        }
    } else {
        output << "time,actor_id,action,object_type,object_id,before,after,result,"
                  "reason,request_id\r\n";
        for (const AuditEntry& entry : audit(viewer_id, event_id, 1000)) {
            output << csv_escape(format_beijing_time(entry.created_at)) << ','
                   << entry.actor_id << ',' << csv_escape(entry.action) << ','
                   << csv_escape(entry.object_type) << ',' << entry.object_id << ','
                   << csv_escape(entry.before_value) << ','
                   << csv_escape(entry.after_value) << ','
                   << csv_escape(entry.result) << ','
                   << csv_escape(entry.reason) << ','
                   << csv_escape(entry.request_id) << "\r\n";
        }
    }
    if (!output.good()) {
        throw BusinessError("CSV 写入失败");
    }
}

}  // namespace ticket
