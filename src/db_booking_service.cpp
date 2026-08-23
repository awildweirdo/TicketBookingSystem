#include "db_booking_service.h"
#include "email.h"

#include <sstream>
#include <vector>
#include <iostream>
#include <iomanip>
#include <optional>
#include <cstring>
#include <cstdlib>

#include <openssl/rand.h>
#include <openssl/sha.h>

namespace
{
    struct TicketDetails
    {
        std::string booking_reference;
        std::string event_title;
        std::string seat_label;
        std::string category;
        int price_cents{0};
    };

    struct SeatCharge
    {
        int event_seat_id{0};
        std::string seat_label;
        std::string category;
        int price_cents{0};
    };

    std::string hex_encode(const unsigned char *data, std::size_t len)
    {
        std::ostringstream oss;
        for (std::size_t i = 0; i < len; ++i)
        {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
        }
        return oss.str();
    }

    std::string sha256_hex(const std::string &value)
    {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest);
        return hex_encode(digest, sizeof(digest));
    }

    std::string random_token()
    {
        unsigned char bytes[16];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        {
            std::memset(bytes, 0, sizeof(bytes));
        }
        return hex_encode(bytes, sizeof(bytes));
    }

    int price_for_category(sqlite3 *db, int event_id, const std::string &category)
    {
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT COALESCE(price_cents, 0) FROM event_prices WHERE event_id = ? AND category = ? LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(stmt);
            return 0;
        }
        sqlite3_bind_int(stmt, 1, event_id);
        sqlite3_bind_text(stmt, 2, category.c_str(), -1, SQLITE_TRANSIENT);
        int price = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            price = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return price;
    }

    std::optional<TicketDetails> create_booking_record(sqlite3 *db, int event_id, int user_id, const std::vector<SeatCharge> &charges)
    {
        if (charges.empty())
        {
            return std::nullopt;
        }

        TicketDetails details;
        sqlite3_stmt *title_stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT title FROM events WHERE id = ? LIMIT 1;", -1, &title_stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int(title_stmt, 1, event_id);
            if (sqlite3_step(title_stmt) == SQLITE_ROW)
            {
                const char *event_title = reinterpret_cast<const char *>(sqlite3_column_text(title_stmt, 0));
                details.event_title = event_title ? event_title : "";
            }
        }
        sqlite3_finalize(title_stmt);

        details.booking_reference = "BK-" + random_token().substr(0, 12);
        std::ostringstream labels;
        int total = 0;
        for (std::size_t i = 0; i < charges.size(); ++i)
        {
            if (i > 0)
            {
                labels << ", ";
            }
            labels << charges[i].seat_label;
            total += charges[i].price_cents;
            if (i == 0)
            {
                details.category = charges[i].category;
            }
        }
        details.seat_label = labels.str();
        details.price_cents = total;

        sqlite3_stmt *booking_stmt = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT INTO bookings(booking_reference, event_id, customer_id, total_cents) VALUES(?, ?, ?, ?);", -1, &booking_stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(booking_stmt);
            return std::nullopt;
        }
        sqlite3_bind_text(booking_stmt, 1, details.booking_reference.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(booking_stmt, 2, event_id);
        sqlite3_bind_int(booking_stmt, 3, user_id);
        sqlite3_bind_int(booking_stmt, 4, details.price_cents);
        if (sqlite3_step(booking_stmt) != SQLITE_DONE)
        {
            sqlite3_finalize(booking_stmt);
            return std::nullopt;
        }
        sqlite3_finalize(booking_stmt);

        const int booking_id = static_cast<int>(sqlite3_last_insert_rowid(db));
        for (const auto &charge : charges)
        {
            sqlite3_stmt *link_stmt = nullptr;
            if (sqlite3_prepare_v2(db, "INSERT INTO booking_seats(booking_id, event_seat_id, price_cents) VALUES(?, ?, ?);", -1, &link_stmt, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(link_stmt);
                return std::nullopt;
            }
            sqlite3_bind_int(link_stmt, 1, booking_id);
            sqlite3_bind_int(link_stmt, 2, charge.event_seat_id);
            sqlite3_bind_int(link_stmt, 3, charge.price_cents);
            if (sqlite3_step(link_stmt) != SQLITE_DONE)
            {
                sqlite3_finalize(link_stmt);
                return std::nullopt;
            }
            sqlite3_finalize(link_stmt);
        }
        return details;
    }

    void send_ticket_email(const std::string &customer_email, const TicketDetails &details)
    {
        const char *api_key_env = std::getenv("SENDGRID_API_KEY");
        const char *from_env = std::getenv("SENDGRID_FROM_EMAIL");
        const char *base_env = std::getenv("APP_BASE_URL");
        const std::string api_key = api_key_env ? api_key_env : std::string();
        const std::string from_email = from_env ? from_env : std::string("noreply@example.com");
        const std::string base_url = base_env ? base_env : std::string("http://localhost:8080");
        const std::string qr_url = build_qr_code_url(details.booking_reference);
        const std::string html = build_ticket_email_html(details.booking_reference, details.event_title, details.seat_label, qr_url);
        std::ostringstream text;
        text << "Your booking is confirmed.\n"
             << "Booking reference: " << details.booking_reference << "\n"
             << "Event: " << details.event_title << "\n"
             << "Seat: " << details.seat_label << "\n"
             << "QR: " << qr_url << "\n"
             << "Base URL: " << base_url << "\n";
        const auto result = send_sendgrid_email(api_key, from_email, customer_email,
                                                "Your ticket for " + details.event_title, html, text.str());
        if (!result.success)
        {
            std::cerr << "[email] " << result.message << "\n";
        }
    }

    void send_waitlist_offer_email(const std::string &customer_email,
                                   const std::string &event_title,
                                   const std::string &category,
                                   int event_id,
                                   const std::string &offer_token)
    {
        const char *api_key_env = std::getenv("SENDGRID_API_KEY");
        const char *from_env = std::getenv("SENDGRID_FROM_EMAIL");
        const char *base_env = std::getenv("APP_BASE_URL");
        const std::string api_key = api_key_env ? api_key_env : std::string();
        const std::string from_email = from_env ? from_env : std::string("noreply@example.com");
        const std::string base_url = base_env ? base_env : std::string("http://localhost:8080");
        const std::string accept_url = base_url + "/waitlist/accept?event_id=" + std::to_string(event_id) + "&token=" + offer_token;
        const std::string html = build_waitlist_offer_email_html(accept_url, category, event_title);
        const std::string text = "A seat is available for your waitlist request. Complete booking here: " + accept_url;
        const auto result = send_sendgrid_email(api_key, from_email, customer_email,
                                                "Seat available for " + event_title, html, text);
        if (!result.success)
        {
            std::cerr << "[email] " << result.message << "\n";
        }
    }
}

DBBookingService::DBBookingService(sqlite3 *db, std::chrono::steady_clock::duration hold_ttl, int event_id)
    : db_(db), hold_ttl_(hold_ttl), event_id_(event_id)
{
}

long DBBookingService::ttl_seconds() const
{
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(hold_ttl_).count();
    if (seconds <= 0)
    {
        seconds = 1;
    }
    return seconds;
}

void DBBookingService::add_seat(const std::string &seat_id, const std::string &category)
{
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO event_seats(event_id, seat_label, category, status) VALUES(?, ?, ?, 'available');", -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    sqlite3_bind_text(stmt, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int DBBookingService::get_or_create_user(const std::string &customer_id) const
{
    sqlite3_stmt *sel = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM users WHERE email = ? LIMIT 1;", -1, &sel, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(sel);
        return 0;
    }
    sqlite3_bind_text(sel, 1, customer_id.c_str(), -1, SQLITE_TRANSIENT);
    int user_id = 0;
    if (sqlite3_step(sel) == SQLITE_ROW)
    {
        user_id = sqlite3_column_int(sel, 0);
    }
    sqlite3_finalize(sel);
    if (user_id != 0)
    {
        return user_id;
    }

    sqlite3_stmt *ins = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO users(email, password_hash, role) VALUES(?, '', 'customer');", -1, &ins, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(ins);
        return 0;
    }
    sqlite3_bind_text(ins, 1, customer_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins) != SQLITE_DONE)
    {
        sqlite3_finalize(ins);
        return 0;
    }
    sqlite3_finalize(ins);
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

HoldResult DBBookingService::hold_seat(const std::string &seat_id, const std::string &customer_id)
{
    release_expired_holds();
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer"};
    }

    char *errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        if (errmsg)
            sqlite3_free(errmsg);
        return {false, "DB busy"};
    }

    const std::string mod = "+" + std::to_string(ttl_seconds()) + " seconds";
    sqlite3_stmt *up = nullptr;
    const char *update_sql =
        "UPDATE event_seats SET status='held', held_by=?, hold_expires_at=datetime('now', ?) "
        "WHERE event_id = ? AND seat_label = ? AND status='available';";
    if (sqlite3_prepare_v2(db_, update_sql, -1, &up, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(up);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error"};
    }
    sqlite3_bind_int(up, 1, user_id);
    sqlite3_bind_text(up, 2, mod.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(up, 3, event_id_);
    sqlite3_bind_text(up, 4, seat_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(up) != SQLITE_DONE)
    {
        sqlite3_finalize(up);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Failed to hold seat"};
    }
    sqlite3_finalize(up);

    if (sqlite3_changes(db_) != 1)
    {
        sqlite3_stmt *sel = nullptr;
        bool exists = false;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM event_seats WHERE event_id = ? AND seat_label = ? LIMIT 1;", -1, &sel, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int(sel, 1, event_id_);
            sqlite3_bind_text(sel, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
            exists = sqlite3_step(sel) == SQLITE_ROW;
        }
        sqlite3_finalize(sel);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, exists ? "Seat is not available" : "Seat does not exist"};
    }

    std::string expires_at;
    sqlite3_stmt *exp = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT hold_expires_at FROM event_seats WHERE event_id = ? AND seat_label = ? LIMIT 1;", -1, &exp, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(exp, 1, event_id_);
        sqlite3_bind_text(exp, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(exp) == SQLITE_ROW)
        {
            const char *text = reinterpret_cast<const char *>(sqlite3_column_text(exp, 0));
            if (text)
                expires_at = text;
        }
    }
    sqlite3_finalize(exp);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    HoldResult result;
    result.success = true;
    result.message = "Seat held";
    result.hold_expires_at = expires_at;
    return result;
}

HoldResult DBBookingService::confirm_seat(const std::string &seat_id, const std::string &customer_id)
{
    release_expired_holds();
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer"};
    }

    char *errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        if (errmsg)
            sqlite3_free(errmsg);
        return {false, "DB busy"};
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, status, held_by, category FROM event_seats WHERE event_id = ? AND seat_label = ? LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error"};
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    sqlite3_bind_text(stmt, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Seat does not exist"};
    }
    const int event_seat_id = sqlite3_column_int(stmt, 0);
    const char *status_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    const int held_by = sqlite3_column_int(stmt, 2);
    const char *category_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    const std::string status = status_c ? status_c : "";
    const std::string category = category_c ? category_c : "";
    sqlite3_finalize(stmt);

    if (status != "held" || held_by != user_id)
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Seat is not held by this customer"};
    }

    sqlite3_stmt *upd = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE event_seats SET status='booked', hold_expires_at = NULL WHERE event_id = ? AND seat_label = ? AND status='held' AND held_by = ?;", -1, &upd, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(upd);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error"};
    }
    sqlite3_bind_int(upd, 1, event_id_);
    sqlite3_bind_text(upd, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(upd, 3, user_id);
    if (sqlite3_step(upd) != SQLITE_DONE || sqlite3_changes(db_) != 1)
    {
        sqlite3_finalize(upd);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Failed to confirm booking"};
    }
    sqlite3_finalize(upd);

    SeatCharge charge{event_seat_id, seat_id, category, price_for_category(db_, event_id_, category)};
    const auto ticket = create_booking_record(db_, event_id_, user_id, {charge});
    if (!ticket.has_value())
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Failed to create booking record"};
    }
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    send_ticket_email(customer_id, *ticket);

    HoldResult result;
    result.success = true;
    result.message = "Booking confirmed";
    result.booking_reference = ticket->booking_reference;
    return result;
}

HoldResult DBBookingService::confirm_held_seats(const std::string &customer_id)
{
    release_expired_holds();
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer"};
    }

    char *errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        if (errmsg)
            sqlite3_free(errmsg);
        return {false, "DB busy"};
    }

    std::vector<SeatCharge> charges;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, seat_label, category FROM event_seats WHERE event_id = ? AND status='held' AND held_by = ?;", -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error"};
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    sqlite3_bind_int(stmt, 2, user_id);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        SeatCharge charge;
        charge.event_seat_id = sqlite3_column_int(stmt, 0);
        const char *label = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        const char *category = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        charge.seat_label = label ? label : "";
        charge.category = category ? category : "";
        charge.price_cents = price_for_category(db_, event_id_, charge.category);
        charges.push_back(std::move(charge));
    }
    sqlite3_finalize(stmt);

    if (charges.empty())
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "No held seats to confirm"};
    }

    for (const auto &charge : charges)
    {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE event_seats SET status='booked', hold_expires_at = NULL WHERE id = ? AND status='held' AND held_by = ?;", -1, &upd, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(upd);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return {false, "DB error"};
        }
        sqlite3_bind_int(upd, 1, charge.event_seat_id);
        sqlite3_bind_int(upd, 2, user_id);
        if (sqlite3_step(upd) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        {
            sqlite3_finalize(upd);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return {false, "Failed to confirm booking"};
        }
        sqlite3_finalize(upd);
    }

    const auto ticket = create_booking_record(db_, event_id_, user_id, charges);
    if (!ticket.has_value())
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Failed to create booking record"};
    }
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    send_ticket_email(customer_id, *ticket);

    HoldResult result;
    result.success = true;
    result.message = "Booking confirmed";
    result.booking_reference = ticket->booking_reference;
    return result;
}

void DBBookingService::mark_related_booking_cancelled(int event_seat_id, int user_id)
{
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "UPDATE bookings SET status='cancelled', cancelled_at=datetime('now') "
        "WHERE customer_id = ? AND status='confirmed' AND id IN "
        "(SELECT booking_id FROM booking_seats WHERE event_seat_id = ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, event_seat_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

WaitlistOfferResult DBBookingService::cancel_booking(const std::string &seat_id, const std::string &customer_id)
{
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer", std::nullopt};
    }

    char *errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        if (errmsg)
            sqlite3_free(errmsg);
        return {false, "DB busy", std::nullopt};
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, status, held_by, category FROM event_seats WHERE event_id = ? AND seat_label = ? LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error", std::nullopt};
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    sqlite3_bind_text(stmt, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Seat does not exist", std::nullopt};
    }
    const int event_seat_id = sqlite3_column_int(stmt, 0);
    const char *status_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    const int held_by = sqlite3_column_int(stmt, 2);
    const char *category_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    const std::string status = status_c ? status_c : "";
    const std::string category = category_c ? category_c : "";
    sqlite3_finalize(stmt);

    if (status != "booked" || held_by != user_id)
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Booking not found", std::nullopt};
    }

    sqlite3_stmt *upd = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE event_seats SET status='available', held_by = NULL, hold_expires_at = NULL WHERE event_id = ? AND seat_label = ?;", -1, &upd, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(upd);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error", std::nullopt};
    }
    sqlite3_bind_int(upd, 1, event_id_);
    sqlite3_bind_text(upd, 2, seat_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(upd) != SQLITE_DONE)
    {
        sqlite3_finalize(upd);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Failed to cancel booking", std::nullopt};
    }
    sqlite3_finalize(upd);

    mark_related_booking_cancelled(event_seat_id, user_id);
    auto offer_token = offer_next_waitlist_for_category(category, event_seat_id);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return {true, offer_token ? "Booking cancelled and waitlist offered" : "Booking cancelled", offer_token};
}

WaitlistOfferResult DBBookingService::cancel_booking_by_id(int booking_id, const std::string &customer_id)
{
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer", std::nullopt};
    }

    char *errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        if (errmsg)
            sqlite3_free(errmsg);
        return {false, "DB busy", std::nullopt};
    }

    sqlite3_stmt *chk = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT event_id FROM bookings WHERE id = ? AND customer_id = ? AND status='confirmed' LIMIT 1;", -1, &chk, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(chk);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error", std::nullopt};
    }
    sqlite3_bind_int(chk, 1, booking_id);
    sqlite3_bind_int(chk, 2, user_id);
    if (sqlite3_step(chk) != SQLITE_ROW)
    {
        sqlite3_finalize(chk);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Booking not found", std::nullopt};
    }
    sqlite3_finalize(chk);

    struct FreedSeat
    {
        int event_seat_id;
        std::string category;
        int event_id;
    };
    std::vector<FreedSeat> freed;
    sqlite3_stmt *seats = nullptr;
    const char *seats_sql =
        "SELECT es.id, es.category, es.event_id FROM booking_seats bs "
        "JOIN event_seats es ON es.id = bs.event_seat_id WHERE bs.booking_id = ?;";
    if (sqlite3_prepare_v2(db_, seats_sql, -1, &seats, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(seats);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error", std::nullopt};
    }
    sqlite3_bind_int(seats, 1, booking_id);
    while (sqlite3_step(seats) == SQLITE_ROW)
    {
        FreedSeat seat;
        seat.event_seat_id = sqlite3_column_int(seats, 0);
        const char *cat = reinterpret_cast<const char *>(sqlite3_column_text(seats, 1));
        seat.category = cat ? cat : "";
        seat.event_id = sqlite3_column_int(seats, 2);
        freed.push_back(std::move(seat));
    }
    sqlite3_finalize(seats);

    for (const auto &seat : freed)
    {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE event_seats SET status='available', held_by = NULL, hold_expires_at = NULL WHERE id = ? AND status='booked';", -1, &upd, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int(upd, 1, seat.event_seat_id);
            sqlite3_step(upd);
        }
        sqlite3_finalize(upd);
    }

    sqlite3_stmt *bk = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE bookings SET status='cancelled', cancelled_at=datetime('now') WHERE id = ?;", -1, &bk, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(bk, 1, booking_id);
        sqlite3_step(bk);
    }
    sqlite3_finalize(bk);

    std::optional<std::string> first_token;
    const int original_event = event_id_;
    for (const auto &seat : freed)
    {
        event_id_ = seat.event_id;
        auto token = offer_next_waitlist_for_category(seat.category, seat.event_seat_id);
        if (token && !first_token)
        {
            first_token = token;
        }
    }
    event_id_ = original_event;

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    return {true, first_token ? "Booking cancelled and waitlist offered" : "Booking cancelled", first_token};
}

WaitlistJoinResult DBBookingService::join_waitlist(const std::string &category, const std::string &customer_id)
{
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer", 0};
    }

    expire_waitlist_offers();

    sqlite3_stmt *avail = nullptr;
    int available = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM event_seats WHERE event_id = ? AND category = ? AND status = 'available';", -1, &avail, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(avail, 1, event_id_);
        sqlite3_bind_text(avail, 2, category.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(avail) == SQLITE_ROW)
        {
            available = sqlite3_column_int(avail, 0);
        }
    }
    sqlite3_finalize(avail);
    if (available > 0)
    {
        return {false, "Category is not sold out", 0};
    }

    sqlite3_stmt *dup = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM waitlist_entries WHERE event_id = ? AND customer_id = ? AND category = ? AND status IN ('waiting', 'offered') LIMIT 1;", -1, &dup, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(dup, 1, event_id_);
        sqlite3_bind_int(dup, 2, user_id);
        sqlite3_bind_text(dup, 3, category.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(dup) == SQLITE_ROW)
        {
            const int existing = sqlite3_column_int(dup, 0);
            sqlite3_finalize(dup);
            return {false, "Already on waitlist for this category", existing};
        }
    }
    sqlite3_finalize(dup);

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO waitlist_entries(event_id, customer_id, category, status) VALUES(?, ?, ?, 'waiting');", -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return {false, "DB error", 0};
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, category.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        return {false, "Failed to join waitlist", 0};
    }
    sqlite3_finalize(stmt);
    return {true, "Joined waitlist", static_cast<int>(sqlite3_last_insert_rowid(db_))};
}

HoldResult DBBookingService::accept_waitlist_offer(const std::string &offer_token, const std::string &customer_id)
{
    const int user_id = get_or_create_user(customer_id);
    if (user_id == 0)
    {
        return {false, "Failed to resolve customer"};
    }

    char *errmsg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        if (errmsg)
            sqlite3_free(errmsg);
        return {false, "DB busy"};
    }

    const std::string token_hash = sha256_hex(offer_token);
    sqlite3_stmt *offer_stmt = nullptr;
    const char *offer_sql =
        "SELECT wo.id, wo.waitlist_entry_id, we.category, we.customer_id, wo.expires_at, wo.completed_at, wo.event_seat_id "
        "FROM waitlist_offers wo JOIN waitlist_entries we ON we.id = wo.waitlist_entry_id "
        "WHERE wo.token_hash = ? AND we.event_id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(db_, offer_sql, -1, &offer_stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(offer_stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error"};
    }
    sqlite3_bind_text(offer_stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(offer_stmt, 2, event_id_);
    if (sqlite3_step(offer_stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(offer_stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Offer not found"};
    }

    const int offer_id = sqlite3_column_int(offer_stmt, 0);
    const int waitlist_entry_id = sqlite3_column_int(offer_stmt, 1);
    const char *category_c = reinterpret_cast<const char *>(sqlite3_column_text(offer_stmt, 2));
    const int offered_customer_id = sqlite3_column_int(offer_stmt, 3);
    const char *expires_at_c = reinterpret_cast<const char *>(sqlite3_column_text(offer_stmt, 4));
    const char *completed_at_c = reinterpret_cast<const char *>(sqlite3_column_text(offer_stmt, 5));
    const int event_seat_id = sqlite3_column_int(offer_stmt, 6);
    const std::string category = category_c ? category_c : "";
    sqlite3_finalize(offer_stmt);

    if (offered_customer_id != user_id)
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Offer belongs to a different customer"};
    }
    if (completed_at_c != nullptr)
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Offer already completed"};
    }
    if (expires_at_c != nullptr)
    {
        sqlite3_stmt *exp_check = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT datetime(?) <= datetime('now');", -1, &exp_check, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(exp_check, 1, expires_at_c, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(exp_check) == SQLITE_ROW && sqlite3_column_int(exp_check, 0) == 1)
            {
                sqlite3_finalize(exp_check);
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
                return {false, "Offer has expired"};
            }
        }
        sqlite3_finalize(exp_check);
    }

    std::string seat_label;
    sqlite3_stmt *label_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT seat_label FROM event_seats WHERE id = ? LIMIT 1;", -1, &label_stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(label_stmt, 1, event_seat_id);
        if (sqlite3_step(label_stmt) == SQLITE_ROW)
        {
            const char *label = reinterpret_cast<const char *>(sqlite3_column_text(label_stmt, 0));
            seat_label = label ? label : "";
        }
    }
    sqlite3_finalize(label_stmt);

    sqlite3_stmt *seat_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE event_seats SET status='booked', held_by = ?, hold_expires_at = NULL WHERE event_id = ? AND id = ? AND status = 'available';", -1, &seat_stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(seat_stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "DB error"};
    }
    sqlite3_bind_int(seat_stmt, 1, user_id);
    sqlite3_bind_int(seat_stmt, 2, event_id_);
    sqlite3_bind_int(seat_stmt, 3, event_seat_id);
    if (sqlite3_step(seat_stmt) != SQLITE_DONE || sqlite3_changes(db_) != 1)
    {
        sqlite3_finalize(seat_stmt);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Seat is no longer available"};
    }
    sqlite3_finalize(seat_stmt);

    SeatCharge charge{event_seat_id, seat_label, category, price_for_category(db_, event_id_, category)};
    const auto ticket = create_booking_record(db_, event_id_, user_id, {charge});
    if (!ticket.has_value())
    {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {false, "Failed to create booking record"};
    }

    sqlite3_stmt *entry_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE waitlist_entries SET status='fulfilled' WHERE id = ?;", -1, &entry_stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(entry_stmt, 1, waitlist_entry_id);
        sqlite3_step(entry_stmt);
    }
    sqlite3_finalize(entry_stmt);

    sqlite3_stmt *offer_upd = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE waitlist_offers SET completed_at = datetime('now') WHERE id = ?;", -1, &offer_upd, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(offer_upd, 1, offer_id);
        sqlite3_step(offer_upd);
    }
    sqlite3_finalize(offer_upd);

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    send_ticket_email(customer_id, *ticket);

    HoldResult result;
    result.success = true;
    result.message = "Waitlist offer accepted";
    result.booking_reference = ticket->booking_reference;
    return result;
}

std::size_t DBBookingService::release_expired_holds()
{
    sqlite3_stmt *stmt = nullptr;
    const char *sql = event_id_ > 0
                          ? "UPDATE event_seats SET status='available', held_by = NULL, hold_expires_at = NULL WHERE event_id = ? AND status = 'held' AND datetime(hold_expires_at) <= datetime('now');"
                          : "UPDATE event_seats SET status='available', held_by = NULL, hold_expires_at = NULL WHERE status = 'held' AND datetime(hold_expires_at) <= datetime('now');";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return 0;
    }
    if (event_id_ > 0)
    {
        sqlite3_bind_int(stmt, 1, event_id_);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

std::vector<std::string> DBBookingService::expire_waitlist_offers()
{
    struct ExpiredOffer
    {
        int entry_id{0};
        int event_seat_id{0};
        int event_id{0};
        std::string category;
    };

    std::vector<ExpiredOffer> expired;
    sqlite3_stmt *sel = nullptr;
    const char *sel_sql =
        "SELECT we.id, wo.event_seat_id, we.event_id, we.category "
        "FROM waitlist_entries we "
        "JOIN waitlist_offers wo ON wo.waitlist_entry_id = we.id "
        "WHERE we.status = 'offered' AND wo.completed_at IS NULL "
        "AND datetime(wo.expires_at) <= datetime('now') "
        "AND (? <= 0 OR we.event_id = ?);";
    if (sqlite3_prepare_v2(db_, sel_sql, -1, &sel, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(sel);
        return {};
    }
    sqlite3_bind_int(sel, 1, event_id_);
    sqlite3_bind_int(sel, 2, event_id_);
    while (sqlite3_step(sel) == SQLITE_ROW)
    {
        ExpiredOffer row;
        row.entry_id = sqlite3_column_int(sel, 0);
        row.event_seat_id = sqlite3_column_int(sel, 1);
        row.event_id = sqlite3_column_int(sel, 2);
        const char *cat = reinterpret_cast<const char *>(sqlite3_column_text(sel, 3));
        row.category = cat ? cat : "";
        expired.push_back(std::move(row));
    }
    sqlite3_finalize(sel);

    std::vector<std::string> new_tokens;
    const int original_event_id = event_id_;
    for (const auto &row : expired)
    {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE waitlist_entries SET status='expired' WHERE id = ? AND status='offered';", -1, &upd, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int(upd, 1, row.entry_id);
            sqlite3_step(upd);
        }
        sqlite3_finalize(upd);

        event_id_ = row.event_id;
        auto token = offer_next_waitlist_for_category(row.category, row.event_seat_id);
        if (token)
        {
            new_tokens.push_back(*token);
        }
    }
    event_id_ = original_event_id;
    return new_tokens;
}

void DBBookingService::run_maintenance()
{
    release_expired_holds();
    expire_waitlist_offers();
}

std::optional<std::string> DBBookingService::offer_next_waitlist_for_category(const std::string &category, int event_seat_id)
{
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT we.id, u.email, e.title FROM waitlist_entries we "
        "JOIN users u ON u.id = we.customer_id JOIN events e ON e.id = we.event_id "
        "WHERE we.event_id = ? AND we.category = ? AND we.status = 'waiting' "
        "ORDER BY we.joined_at ASC, we.id ASC LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    sqlite3_bind_text(stmt, 2, category.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    const int waitlist_entry_id = sqlite3_column_int(stmt, 0);
    const char *customer_email_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    const char *event_title_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    const std::string customer_email = customer_email_c ? customer_email_c : "";
    const std::string event_title = event_title_c ? event_title_c : "";
    sqlite3_finalize(stmt);

    const std::string token = random_token();
    const std::string token_hash = sha256_hex(token);
    const std::string mod = "+" + std::to_string(ttl_seconds()) + " seconds";

    sqlite3_stmt *upd = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE waitlist_entries SET status='offered' WHERE id = ?;", -1, &upd, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(upd);
        return std::nullopt;
    }
    sqlite3_bind_int(upd, 1, waitlist_entry_id);
    if (sqlite3_step(upd) != SQLITE_DONE)
    {
        sqlite3_finalize(upd);
        return std::nullopt;
    }
    sqlite3_finalize(upd);

    sqlite3_stmt *offer_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO waitlist_offers(waitlist_entry_id, event_seat_id, token_hash, expires_at) VALUES(?, ?, ?, datetime('now', ?));", -1, &offer_stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(offer_stmt);
        return std::nullopt;
    }
    sqlite3_bind_int(offer_stmt, 1, waitlist_entry_id);
    sqlite3_bind_int(offer_stmt, 2, event_seat_id);
    sqlite3_bind_text(offer_stmt, 3, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(offer_stmt, 4, mod.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(offer_stmt) != SQLITE_DONE)
    {
        sqlite3_finalize(offer_stmt);
        return std::nullopt;
    }
    sqlite3_finalize(offer_stmt);

    send_waitlist_offer_email(customer_email, event_title, category, event_id_, token);
    return token;
}

std::vector<Seat> DBBookingService::seats() const
{
    std::vector<Seat> result;
    const char *sql =
        "SELECT es.seat_label, es.category, es.status, es.held_by, es.hold_expires_at, "
        "COALESCE(vs.row_number, 0), COALESCE(vs.column_number, 0), COALESCE(ep.price_cents, 0) "
        "FROM event_seats es "
        "JOIN events e ON e.id = es.event_id "
        "LEFT JOIN venue_seats vs ON vs.venue_id = e.venue_id AND vs.seat_label = es.seat_label "
        "LEFT JOIN event_prices ep ON ep.event_id = es.event_id AND ep.category = es.category "
        "WHERE es.event_id = ? "
        "ORDER BY COALESCE(vs.row_number, 999), COALESCE(vs.column_number, 999), es.seat_label;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return result;
    }
    sqlite3_bind_int(stmt, 1, event_id_);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Seat s;
        const char *label = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        const char *category = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        const char *status = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        const int held_by = sqlite3_column_int(stmt, 3);
        const char *expires = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        s.id = label ? label : "";
        s.category = category ? category : "";
        if (status)
        {
            const std::string st(status);
            if (st == "available")
                s.status = SeatStatus::Available;
            else if (st == "held")
                s.status = SeatStatus::Held;
            else if (st == "booked")
                s.status = SeatStatus::Booked;
        }
        s.held_by_id = held_by;
        if (held_by != 0)
        {
            s.holder = std::to_string(held_by);
        }
        if (expires)
        {
            s.hold_expires_at_iso = expires;
        }
        s.row_number = sqlite3_column_int(stmt, 5);
        s.column_number = sqlite3_column_int(stmt, 6);
        s.price_cents = sqlite3_column_int(stmt, 7);
        result.push_back(std::move(s));
    }
    sqlite3_finalize(stmt);
    return result;
}
