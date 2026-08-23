#include "booking_service.h"
#include "db.h"
#include "db_booking_service.h"

#include "crow.h"
#include "auth.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <thread>
#include <atomic>

namespace
{
    std::string status_name(const SeatStatus status)
    {
        switch (status)
        {
        case SeatStatus::Available:
            return "available";
        case SeatStatus::Held:
            return "held";
        case SeatStatus::Booked:
            return "booked";
        }
        return "unknown";
    }
}


void seed_admin(sqlite3* db) {
    sqlite3_stmt *stmt = nullptr;
    const char* check_sql = "SELECT id FROM users WHERE email = 'admin@admin.com';";
    if (sqlite3_prepare_v2(db, check_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            const char* insert_sql = "INSERT INTO users (email, password_hash, role) VALUES (?, ?, 'admin');";
            if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, "admin@admin.com", -1, SQLITE_STATIC);
                std::string hashed = auth::hash_password("password123");
                sqlite3_bind_text(stmt, 2, hashed.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                std::cout << "Seeded default admin account: admin@admin.com / password123" << std::endl;
            }
        }
        sqlite3_finalize(stmt);
    }
}

int main()
{
    const char* db_path_env = std::getenv("DB_PATH");
    std::string db_path = db_path_env ? db_path_env : "booking.db";
    DB db(db_path);
    std::string db_err;
    if (!db.init_schema("database/schema.sql", db_err))
    {
        std::cerr << "Database initialization failed: " << db_err << std::endl;
        return 1;
    }

    BookingService booking_service(std::chrono::minutes(10));
    std::unique_ptr<DBBookingService> db_booking_service;
    // If DB is available, seed event seats into DB-backed service
    if (db.get())
    {
        char *seed_err = nullptr;
        sqlite3_exec(db.get(), "INSERT OR IGNORE INTO venues(id, name) VALUES(1, 'Demo Venue');", nullptr, nullptr, &seed_err);
        if (seed_err) { sqlite3_free(seed_err); seed_err = nullptr; }
        sqlite3_exec(db.get(), "INSERT OR IGNORE INTO users(id, email, password_hash, role) VALUES(1, 'organiser@demo.local', '', 'organiser');", nullptr, nullptr, &seed_err);
        if (seed_err) { sqlite3_free(seed_err); seed_err = nullptr; }
        sqlite3_exec(db.get(), "INSERT OR IGNORE INTO events(id, organiser_id, venue_id, title, event_type, starts_at) VALUES(1, 1, 1, 'Demo Movie Night', 'movie', datetime('now'));", nullptr, nullptr, &seed_err);
        if (seed_err) { sqlite3_free(seed_err); seed_err = nullptr; }

        db_booking_service = std::make_unique<DBBookingService>(db.get(), std::chrono::minutes(10));
        db_booking_service->add_seat("A1", "Standard");
        db_booking_service->add_seat("A2", "Premium");
    }
    else
    {
        booking_service.add_seat("A1", "Standard");
        booking_service.add_seat("A2", "Premium");
    }

    crow::SimpleApp app;

    crow::mustache::set_base("public");

    CROW_ROUTE(app, "/")([]
                         { 
        crow::response res;
        res.set_static_file_info("public/index.html");
        return res;
 });

    CROW_ROUTE(app, "/api/health")([]
                                   {
        crow::json::wvalue response;
        response["status"] = "ok";
        response["service"] = "ticket-booking-system";
        return response; });

    CROW_ROUTE(app, "/api/events/<int>/seats")([&db](int event_id)
                                  {
        crow::json::wvalue response;
        if (!db.get()) return crow::json::wvalue();
        DBBookingService event_service(db.get(), std::chrono::minutes(10), event_id);
        const auto seats = event_service.seats();
        crow::json::wvalue seat_list;
        for (std::size_t index = 0; index < seats.size(); ++index)
        {
            const auto &seat = seats[index];
            crow::json::wvalue item;
            item["id"] = seat.id;
            item["category"] = seat.category;
            item["status"] = status_name(seat.status);
            item["row_number"] = seat.row_number;
            item["column_number"] = seat.column_number;
            item["price_cents"] = seat.price_cents;
            seat_list[static_cast<unsigned>(index)] = std::move(item);
        }
        response["seats"] = std::move(seat_list);
        return response; });

    // Helper: resolve user id from JWT to email stored in DB
    auto get_email_for_user = [&db](int user_id) -> std::optional<std::string>
    {
        if (!db.get())
            return std::nullopt;
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT email FROM users WHERE id = ? LIMIT 1;";
        if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        sqlite3_bind_int(stmt, 1, user_id);
        const int rc = sqlite3_step(stmt);
        std::optional<std::string> result;
        if (rc == SQLITE_ROW)
        {
            const unsigned char *text = sqlite3_column_text(stmt, 0);
            if (text) result = std::string(reinterpret_cast<const char *>(text));
        }
        sqlite3_finalize(stmt);
        return result;
    };

    auto jwt_secret = []() -> std::string
    {
        const char *secret_env = std::getenv("JWT_SECRET");
        return secret_env ? std::string(secret_env) : std::string("dev-secret");
    };

    auto require_role = [&](const crow::request &request, const std::vector<std::string> &roles) -> std::optional<JwtPayload>
    {
        const std::string auth_hdr = request.get_header_value("Authorization");
        if (auth_hdr.empty() || auth_hdr.rfind("Bearer ", 0) != 0)
        {
            return std::nullopt;
        }
        const auto payload = auth::verify_jwt(auth_hdr.substr(7), jwt_secret());
        if (!payload.has_value())
        {
            return std::nullopt;
        }
        for (const auto &role : roles)
        {
            if (payload->role == role)
            {
                return payload;
            }
        }
        return std::nullopt;
    };

    auto db_stmt_exec = [&db](const std::string &sql) -> bool
    {
        char *err = nullptr;
        const int rc = sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, &err);
        if (err)
        {
            sqlite3_free(err);
        }
        return rc == SQLITE_OK;
    };

    auto execute_insert = [&db](const std::string &sql, const std::vector<std::string> &values) -> std::optional<int>
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), values[i].c_str(), -1, SQLITE_TRANSIENT);
        }
        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
        const int row_id = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
        sqlite3_finalize(stmt);
        return row_id;
    };

    CROW_ROUTE(app, "/api/register").methods(crow::HTTPMethod::POST)([&db](const crow::request &request)
                                                                       {
            const auto body = crow::json::load(request.body);
            if (!body || !body.has("email") || !body.has("password") || body["email"].t() != crow::json::type::String || body["password"].t() != crow::json::type::String)
            {
                return crow::response(400, "email and password are required");
            }
            if (!db.get())
            {
                return crow::response(500, "database not available");
            }
            const std::string email = body["email"].s();
            const std::string password = body["password"].s();
            std::string role = "customer";
            if (body.has("role") && body["role"].t() == crow::json::type::String)
                role = body["role"].s();

            const std::string pwd_hash = auth::hash_password(password);

            sqlite3_stmt *stmt = nullptr;
            const char *sql = "INSERT INTO users(email, password_hash, role) VALUES(?, ?, ?);";
            if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(stmt);
                return crow::response(500, "db error");
            }
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pwd_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE)
            {
                const int err = sqlite3_errcode(db.get());
                sqlite3_finalize(stmt);
                if (err == SQLITE_CONSTRAINT)
                {
                    crow::json::wvalue resp;
                    resp["success"] = false;
                    resp["message"] = "email already registered";
                    return crow::response(409, resp);
                }
                return crow::response(500, "db error");
            }
            const int user_id = static_cast<int>(sqlite3_last_insert_rowid(db.get()));
            sqlite3_finalize(stmt);
            crow::json::wvalue resp;
            resp["success"] = true;
            resp["user_id"] = user_id;
            return crow::response(201, resp);
        });

    CROW_ROUTE(app, "/api/login").methods(crow::HTTPMethod::POST)([&db](const crow::request &request)
                                                                    {
            const auto body = crow::json::load(request.body);
            if (!body || !body.has("email") || !body.has("password") || body["email"].t() != crow::json::type::String || body["password"].t() != crow::json::type::String)
            {
                return crow::response(400, "email and password are required");
            }
            if (!db.get())
            {
                return crow::response(500, "database not available");
            }
            const std::string email = body["email"].s();
            const std::string password = body["password"].s();

            sqlite3_stmt *stmt = nullptr;
            const char *sql = "SELECT id, password_hash, role FROM users WHERE email = ? LIMIT 1;";
            if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(stmt);
                return crow::response(500, "db error");
            }
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt);
            if (rc != SQLITE_ROW)
            {
                sqlite3_finalize(stmt);
                return crow::response(401, "invalid credentials");
            }
            const int user_id = sqlite3_column_int(stmt, 0);
            const char *pwd_hash_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            const char *role_c = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            const std::string pwd_hash = pwd_hash_c ? pwd_hash_c : std::string();
            const std::string role = role_c ? role_c : std::string("customer");
            sqlite3_finalize(stmt);
            if (!auth::verify_password(password, pwd_hash))
            {
                return crow::response(401, "invalid credentials");
            }
            const char *secret_env = std::getenv("JWT_SECRET");
            const std::string secret = secret_env ? secret_env : std::string("dev-secret");
            const char *ttl_env = std::getenv("JWT_TTL");
            const int ttl = ttl_env ? std::atoi(ttl_env) : 3600;
            const std::string token = auth::issue_jwt(user_id, role, secret, ttl);
            crow::json::wvalue resp;
            resp["success"] = true;
            resp["token"] = token;
            resp["role"] = role;
            resp["user_id"] = user_id;
            return crow::response(200, resp);
        });

    CROW_ROUTE(app, "/api/venues").methods(crow::HTTPMethod::POST)([&db, &require_role, &execute_insert, &db_stmt_exec](const crow::request &request)
                                                                      {
            const auto auth = require_role(request, {"admin"});
            if (!auth.has_value())
            {
                return crow::response(403, "admin role required");
            }

            const auto body = crow::json::load(request.body);
            if (!body || !body.has("name") || body["name"].t() != crow::json::type::String)
            {
                return crow::response(400, "name is required");
            }

            const std::string name = body["name"].s();
            const auto venue_id = execute_insert("INSERT INTO venues(name) VALUES(?);", {name});
            if (!venue_id.has_value())
            {
                return crow::response(500, "failed to create venue");
            }

            if (body.has("seats") && body["seats"].t() == crow::json::type::List)
            {
                const auto seat_count = body["seats"].size();
                for (std::size_t i = 0; i < seat_count; ++i)
                {
                    const auto &seat = body["seats"][static_cast<int>(i)];
                    if (seat.t() != crow::json::type::Object || !seat.has("seat_label") || !seat.has("category") || !seat.has("row_number") || !seat.has("column_number"))
                    {
                        return crow::response(400, "invalid seat layout item");
                    }
                    sqlite3_stmt *stmt = nullptr;
                    const char *sql = "INSERT INTO venue_seats(venue_id, seat_label, category, row_number, column_number) VALUES(?, ?, ?, ?, ?);";
                    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
                    {
                        sqlite3_finalize(stmt);
                        return crow::response(500, "db error");
                    }
                    sqlite3_bind_int(stmt, 1, *venue_id);
                    const std::string seat_label = seat["seat_label"].s();
                    const std::string seat_category = seat["category"].s();
                    sqlite3_bind_text(stmt, 2, seat_label.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, seat_category.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 4, static_cast<int>(seat["row_number"].i()));
                    sqlite3_bind_int(stmt, 5, static_cast<int>(seat["column_number"].i()));
                    if (sqlite3_step(stmt) != SQLITE_DONE)
                    {
                        sqlite3_finalize(stmt);
                        return crow::response(500, "failed to create venue seats");
                    }
                    sqlite3_finalize(stmt);
                }
            }

            crow::json::wvalue resp;
            resp["success"] = true;
            resp["venue_id"] = *venue_id;
            return crow::response(201, resp);
        });

    CROW_ROUTE(app, "/api/venues").methods(crow::HTTPMethod::GET)([&db]
                                                                    {
            crow::json::wvalue response;
            crow::json::wvalue venue_list;
            sqlite3_stmt *stmt = nullptr;
            const char *sql = "SELECT id, name FROM venues ORDER BY id ASC;";
            if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(stmt);
                return crow::response(500, "db error");
            }
            std::size_t index = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                crow::json::wvalue item;
                item["id"] = sqlite3_column_int(stmt, 0);
                const unsigned char *name = sqlite3_column_text(stmt, 1);
                item["name"] = name ? reinterpret_cast<const char *>(name) : "";
                venue_list[static_cast<unsigned>(index++)] = std::move(item);
            }
            sqlite3_finalize(stmt);
            response["venues"] = std::move(venue_list);
            return crow::response(response); });

    CROW_ROUTE(app, "/api/events").methods(crow::HTTPMethod::POST)([&db, &require_role, &execute_insert](const crow::request &request)
                                                                     {
            const auto auth = require_role(request, {"organiser", "admin"});
            if (!auth.has_value())
            {
                return crow::response(403, "organiser or admin role required");
            }

            const auto body = crow::json::load(request.body);
            if (!body || !body.has("venue_id") || !body.has("title") || !body.has("event_type") || !body.has("starts_at")
                || body["title"].t() != crow::json::type::String || body["event_type"].t() != crow::json::type::String || body["starts_at"].t() != crow::json::type::String)
            {
                return crow::response(400, "venue_id, title, event_type, and starts_at are required");
            }

            const int venue_id = static_cast<int>(body["venue_id"].i());
            const std::string title = body["title"].s();
            const std::string event_type = body["event_type"].s();
            const std::string starts_at = body["starts_at"].s();
            if (event_type != "movie" && event_type != "concert")
            {
                return crow::response(400, "event_type must be movie or concert");
            }

            sqlite3_stmt *chk_stmt = nullptr;
            const char *chk_sql = "SELECT count(*) FROM events WHERE venue_id = ? AND abs(julianday(starts_at) - julianday(?)) < 3.0/24.0;";
            if (sqlite3_prepare_v2(db.get(), chk_sql, -1, &chk_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(chk_stmt, 1, venue_id);
                sqlite3_bind_text(chk_stmt, 2, starts_at.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(chk_stmt) == SQLITE_ROW) {
                    if (sqlite3_column_int(chk_stmt, 0) > 0) {
                        sqlite3_finalize(chk_stmt);
                        return crow::response(409, "Double booking: venue is already booked for an event near this time (within 3 hours).");
                    }
                }
                sqlite3_finalize(chk_stmt);
            }

            auto event_id = execute_insert("INSERT INTO events(organiser_id, venue_id, title, event_type, starts_at) VALUES(?, ?, ?, ?, ?);",
                                           {std::to_string(auth->user_id), std::to_string(venue_id), title, event_type, starts_at});
            if (!event_id.has_value())
            {
                return crow::response(500, "failed to create event");
            }

            if (body.has("prices") && body["prices"].t() == crow::json::type::List)
            {
                for (std::size_t i = 0; i < body["prices"].size(); ++i)
                {
                    const auto &price = body["prices"][static_cast<int>(i)];
                    if (price.t() != crow::json::type::Object || !price.has("category") || !price.has("price_cents"))
                    {
                        return crow::response(400, "invalid price item");
                    }
                    sqlite3_stmt *stmt = nullptr;
                    const char *sql = "INSERT INTO event_prices(event_id, category, price_cents) VALUES(?, ?, ?);";
                    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
                    {
                        sqlite3_finalize(stmt);
                        return crow::response(500, "db error");
                    }
                    sqlite3_bind_int(stmt, 1, *event_id);
                    const std::string price_category = price["category"].s();
                    sqlite3_bind_text(stmt, 2, price_category.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 3, static_cast<int>(price["price_cents"].i()));
                    if (sqlite3_step(stmt) != SQLITE_DONE)
                    {
                        sqlite3_finalize(stmt);
                        return crow::response(500, "failed to create event pricing");
                    }
                    sqlite3_finalize(stmt);
                }
            }

            sqlite3_stmt *seats_stmt = nullptr;
            const char *seats_sql = "SELECT seat_label, category FROM venue_seats WHERE venue_id = ? ORDER BY id ASC;";
            if (sqlite3_prepare_v2(db.get(), seats_sql, -1, &seats_stmt, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(seats_stmt);
                return crow::response(500, "db error");
            }
            sqlite3_bind_int(seats_stmt, 1, venue_id);
            while (sqlite3_step(seats_stmt) == SQLITE_ROW)
            {
                const char *seat_label = reinterpret_cast<const char *>(sqlite3_column_text(seats_stmt, 0));
                const char *category = reinterpret_cast<const char *>(sqlite3_column_text(seats_stmt, 1));
                sqlite3_stmt *stmt = nullptr;
                const char *sql = "INSERT INTO event_seats(event_id, seat_label, category, status) VALUES(?, ?, ?, 'available');";
                if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
                {
                    sqlite3_finalize(stmt);
                    sqlite3_finalize(seats_stmt);
                    return crow::response(500, "db error");
                }
                sqlite3_bind_int(stmt, 1, *event_id);
                sqlite3_bind_text(stmt, 2, seat_label ? seat_label : "", -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, category ? category : "", -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) != SQLITE_DONE)
                {
                    sqlite3_finalize(stmt);
                    sqlite3_finalize(seats_stmt);
                    return crow::response(500, "failed to seed event seats");
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_finalize(seats_stmt);

            crow::json::wvalue resp;
            resp["success"] = true;
            resp["event_id"] = *event_id;
            return crow::response(201, resp);
        });

    CROW_ROUTE(app, "/api/events").methods(crow::HTTPMethod::GET)([&db]
                                                                    {
            crow::json::wvalue response;
            crow::json::wvalue event_list;
            sqlite3_stmt *stmt = nullptr;
            const char *sql = "SELECT id, title, event_type, starts_at, venue_id, organiser_id FROM events ORDER BY id ASC;";
            if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(stmt);
                return crow::response(500, "db error");
            }
            std::size_t index = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                crow::json::wvalue item;
                item["id"] = sqlite3_column_int(stmt, 0);
                const unsigned char *title = sqlite3_column_text(stmt, 1);
                const unsigned char *event_type = sqlite3_column_text(stmt, 2);
                const unsigned char *starts_at = sqlite3_column_text(stmt, 3);
                item["title"] = title ? reinterpret_cast<const char *>(title) : "";
                item["event_type"] = event_type ? reinterpret_cast<const char *>(event_type) : "";
                item["starts_at"] = starts_at ? reinterpret_cast<const char *>(starts_at) : "";
                item["venue_id"] = sqlite3_column_int(stmt, 4);
                item["organiser_id"] = sqlite3_column_int(stmt, 5);
                event_list[static_cast<unsigned>(index++)] = std::move(item);
            }
            sqlite3_finalize(stmt);
            response["events"] = std::move(event_list);
            return crow::response(response); });


    CROW_ROUTE(app, "/api/events/<int>/summary").methods(crow::HTTPMethod::GET)([&db, &require_role](const crow::request &request, int event_id)
    {
        const auto auth = require_role(request, {"organiser", "admin"});
        if (!auth.has_value()) return crow::response(403, "organiser or admin role required");
        
        if (auth->role == "organiser") {
            sqlite3_stmt *chk = nullptr;
            if (sqlite3_prepare_v2(db.get(), "SELECT organiser_id FROM events WHERE id = ?;", -1, &chk, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(chk, 1, event_id);
                if (sqlite3_step(chk) == SQLITE_ROW) {
                    if (sqlite3_column_int(chk, 0) != auth->user_id) {
                        sqlite3_finalize(chk);
                        return crow::response(403, "you can only view your own events");
                    }
                }
                sqlite3_finalize(chk);
            }
        }

        crow::json::wvalue response;
        sqlite3_stmt *stmt = nullptr;
        // Total revenue
        const char *sql1 = "SELECT COALESCE(SUM(total_cents), 0) FROM bookings WHERE event_id = ? AND status = 'confirmed';";
        if (sqlite3_prepare_v2(db.get(), sql1, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, event_id);
            if (sqlite3_step(stmt) == SQLITE_ROW) response["total_revenue_cents"] = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        
        // Seat counts
        const char *sql2 = "SELECT status, COUNT(*) FROM event_seats WHERE event_id = ? GROUP BY status;";
        if (sqlite3_prepare_v2(db.get(), sql2, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, event_id);
            crow::json::wvalue counts;
            int total = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                int count = sqlite3_column_int(stmt, 1);
                counts[st ? st : "unknown"] = count;
                total += count;
            }
            response["seat_counts"] = std::move(counts);
            response["total_seats"] = total;
            sqlite3_finalize(stmt);
        }
        return crow::response(response);
    });

    CROW_ROUTE(app, "/api/events/<int>/waitlist").methods(crow::HTTPMethod::POST)([&db, &get_email_for_user](const crow::request &request, int event_id)
                                                                                     {
            if (!db.get())
            {
                return crow::response(500, "database not available");
            }
            const auto body = crow::json::load(request.body);
            if (!body || !body.has("category") || body["category"].t() != crow::json::type::String)
            {
                return crow::response(400, "category is required");
            }

            std::string customer_identifier;
            const std::string auth_hdr = request.get_header_value("Authorization");
            if (!auth_hdr.empty() && auth_hdr.rfind("Bearer ", 0) == 0)
            {
                const char *secret_env = std::getenv("JWT_SECRET");
                const std::string secret = secret_env ? secret_env : std::string("dev-secret");
                const auto payload = auth::verify_jwt(auth_hdr.substr(7), secret);
                if (!payload.has_value())
                {
                    return crow::response(401, "invalid token");
                }
                if (payload->role != "customer")
                {
                    return crow::response(403, "insufficient role");
                }
                auto email_opt = get_email_for_user(payload->user_id);
                if (!email_opt.has_value())
                {
                    return crow::response(500, "user record not found");
                }
                customer_identifier = *email_opt;
            }
            else
            {
                if (!body.has("customer_id") || body["customer_id"].t() != crow::json::type::String)
                {
                    return crow::response(400, "customer_id is required when no Authorization header provided");
                }
                customer_identifier = body["customer_id"].s();
            }

            DBBookingService event_service(db.get(), std::chrono::minutes(10), event_id);
            const auto result = event_service.join_waitlist(body["category"].s(), customer_identifier);
            crow::json::wvalue response;
            response["success"] = result.success;
            response["message"] = result.message;
            response["waitlist_entry_id"] = result.entry_id;
            return crow::response(result.success ? 201 : 409, response); });

    CROW_ROUTE(app, "/api/events/<int>/waitlist/<string>/accept").methods(crow::HTTPMethod::POST)([&db, &get_email_for_user](const crow::request &request, int event_id, const std::string &offer_token)
                                                                                                    {
            if (!db.get())
            {
                return crow::response(500, "database not available");
            }
            const auto body = crow::json::load(request.body);
            std::string customer_identifier;
            const std::string auth_hdr = request.get_header_value("Authorization");
            if (!auth_hdr.empty() && auth_hdr.rfind("Bearer ", 0) == 0)
            {
                const char *secret_env = std::getenv("JWT_SECRET");
                const std::string secret = secret_env ? secret_env : std::string("dev-secret");
                const auto payload = auth::verify_jwt(auth_hdr.substr(7), secret);
                if (!payload.has_value())
                {
                    return crow::response(401, "invalid token");
                }
                if (payload->role != "customer")
                {
                    return crow::response(403, "insufficient role");
                }
                auto email_opt = get_email_for_user(payload->user_id);
                if (!email_opt.has_value())
                {
                    return crow::response(500, "user record not found");
                }
                customer_identifier = *email_opt;
            }
            else
            {
                if (!body || !body.has("customer_id") || body["customer_id"].t() != crow::json::type::String)
                {
                    return crow::response(400, "customer_id is required when no Authorization header provided");
                }
                customer_identifier = body["customer_id"].s();
            }

            DBBookingService event_service(db.get(), std::chrono::minutes(10), event_id);
            const auto result = event_service.accept_waitlist_offer(offer_token, customer_identifier);
            crow::json::wvalue response;
            response["success"] = result.success;
            response["message"] = result.message;
            return crow::response(result.success ? 200 : 409, response); });


    CROW_ROUTE(app, "/api/me/bookings").methods(crow::HTTPMethod::GET)([&db, &get_email_for_user](const crow::request &request)
    {
        const std::string auth_hdr = request.get_header_value("Authorization");
        if (auth_hdr.empty() || auth_hdr.rfind("Bearer ", 0) != 0) return crow::response(401, "unauthorized");
        const char *secret_env = std::getenv("JWT_SECRET");
        const auto payload = auth::verify_jwt(auth_hdr.substr(7), secret_env ? secret_env : "dev-secret");
        if (!payload.has_value() || payload->role != "customer") return crow::response(403, "forbidden");

        crow::json::wvalue response;
        crow::json::wvalue bookings;
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT b.id, b.booking_reference, b.event_id, b.status, b.total_cents, e.title "
                          "FROM bookings b JOIN events e ON e.id = b.event_id "
                          "WHERE b.customer_id = ? ORDER BY b.created_at DESC;";
        if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) return crow::response(500, "db error");
        sqlite3_bind_int(stmt, 1, payload->user_id);
        int index = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            crow::json::wvalue b;
            b["id"] = sqlite3_column_int(stmt, 0);
            b["booking_reference"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            b["event_id"] = sqlite3_column_int(stmt, 2);
            b["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            b["total_cents"] = sqlite3_column_int(stmt, 4);
            b["event_title"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            bookings[index++] = std::move(b);
        }
        sqlite3_finalize(stmt);
        response["bookings"] = std::move(bookings);
        return crow::response(response);
    });

    
    CROW_ROUTE(app, "/api/me/bookings/<int>/cancel").methods(crow::HTTPMethod::POST)([&db, &get_email_for_user](const crow::request &request, int booking_id)
    {
        const std::string auth_hdr = request.get_header_value("Authorization");
        if (auth_hdr.empty() || auth_hdr.rfind("Bearer ", 0) != 0) return crow::response(401, "unauthorized");
        const char *secret_env = std::getenv("JWT_SECRET");
        const auto payload = auth::verify_jwt(auth_hdr.substr(7), secret_env ? secret_env : "dev-secret");
        if (!payload.has_value() || payload->role != "customer") return crow::response(403, "forbidden");

        auto email_opt = get_email_for_user(payload->user_id);
        if (!email_opt.has_value()) return crow::response(500, "user record not found");
        std::string customer_identifier = *email_opt;

        if (!db.get()) return crow::response(500, "database not available");
        // We use event_id 0 here for maintenance operations since cancel_booking_by_id crosses events 
        // or wait, cancel_booking_by_id doesn't rely on event_id being accurate if it looks up by booking_id
        DBBookingService service(db.get(), std::chrono::minutes(10), 0);
        auto cancel_result = service.cancel_booking_by_id(booking_id, customer_identifier);
        
        crow::json::wvalue response;
        response["success"] = cancel_result.success;
        response["message"] = cancel_result.success ? "Booking cancelled" : cancel_result.message;
        if (cancel_result.offer_token.has_value()) {
            response["offer_token"] = *cancel_result.offer_token;
        }
        return crow::response(cancel_result.success ? 200 : 400, response);
    });

    CROW_ROUTE(app, "/api/events/<int>/seats/<string>/hold").methods(crow::HTTPMethod::POST)([&db,get_email_for_user](const crow::request &request, int event_id, const std::string &seat_id)
                                                                                {
            const auto body = crow::json::load(request.body);
            const std::string auth_hdr = request.get_header_value("Authorization");
            std::string customer_identifier;

            if (!auth_hdr.empty() && auth_hdr.rfind("Bearer ", 0) == 0)
            {
                const std::string token = auth_hdr.substr(7);
                const char *secret_env = std::getenv("JWT_SECRET");
                const std::string secret = secret_env ? secret_env : std::string("dev-secret");
                const auto payload = auth::verify_jwt(token, secret);
                if (!payload.has_value())
                {
                    return crow::response(401, "invalid token");
                }
                if (payload->role != "customer")
                {
                    return crow::response(403, "insufficient role");
                }
                auto email_opt = get_email_for_user(payload->user_id);
                if (!email_opt.has_value())
                {
                    return crow::response(500, "user record not found");
                }
                customer_identifier = *email_opt;
            }
            else
            {
                if (!body || !body.has("customer_id") || body["customer_id"].t() != crow::json::type::String)
                {
                    return crow::response(400, "customer_id is required when no Authorization header provided");
                }
                customer_identifier = body["customer_id"].s();
            }

            DBBookingService event_service(db.get(), std::chrono::minutes(10), event_id);
            const auto result = event_service.hold_seat(seat_id, customer_identifier);
            crow::json::wvalue response;
            response["success"] = result.success;
            response["message"] = result.message;
            return crow::response(result.success ? 201 : 409, response);
        });

    CROW_ROUTE(app, "/api/events/<int>/seats/<string>/confirm").methods(crow::HTTPMethod::POST)([&db,get_email_for_user](const crow::request &request, int event_id, const std::string &seat_id)
                                                                                   {
            const auto body = crow::json::load(request.body);
            const std::string auth_hdr = request.get_header_value("Authorization");
            std::string customer_identifier;
            if (!auth_hdr.empty() && auth_hdr.rfind("Bearer ", 0) == 0)
            {
                const std::string token = auth_hdr.substr(7);
                const char *secret_env = std::getenv("JWT_SECRET");
                const std::string secret = secret_env ? secret_env : std::string("dev-secret");
                const auto payload = auth::verify_jwt(token, secret);
                if (!payload.has_value())
                {
                    return crow::response(401, "invalid token");
                }
                if (payload->role != "customer")
                {
                    return crow::response(403, "insufficient role");
                }
                auto email_opt = get_email_for_user(payload->user_id);
                if (!email_opt.has_value())
                {
                    return crow::response(500, "user record not found");
                }
                customer_identifier = *email_opt;
            }
            else
            {
                if (!body || !body.has("customer_id") || body["customer_id"].t() != crow::json::type::String)
                {
                    return crow::response(400, "customer_id is required when no Authorization header provided");
                }
                customer_identifier = body["customer_id"].s();
            }

            DBBookingService event_service(db.get(), std::chrono::minutes(10), event_id);
            const auto result = event_service.confirm_seat(seat_id, customer_identifier);
            crow::json::wvalue response;
            response["success"] = result.success;
            response["message"] = result.message;
            return crow::response(result.success ? 200 : 409, response);
        });

    CROW_ROUTE(app, "/api/events/<int>/seats/<string>/cancel").methods(crow::HTTPMethod::POST)([&db,get_email_for_user](const crow::request &request, int event_id, const std::string &seat_id)
                                                                                  {
            const auto body = crow::json::load(request.body);
            const std::string auth_hdr = request.get_header_value("Authorization");
            std::string customer_identifier;
            if (!auth_hdr.empty() && auth_hdr.rfind("Bearer ", 0) == 0)
            {
                const std::string token = auth_hdr.substr(7);
                const char *secret_env = std::getenv("JWT_SECRET");
                const std::string secret = secret_env ? secret_env : std::string("dev-secret");
                const auto payload = auth::verify_jwt(token, secret);
                if (!payload.has_value())
                {
                    return crow::response(401, "invalid token");
                }
                if (payload->role != "customer")
                {
                    return crow::response(403, "insufficient role");
                }
                auto email_opt = get_email_for_user(payload->user_id);
                if (!email_opt.has_value())
                {
                    return crow::response(500, "user record not found");
                }
                customer_identifier = *email_opt;
            }
            else
            {
                if (!body || !body.has("customer_id") || body["customer_id"].t() != crow::json::type::String)
                {
                    return crow::response(400, "customer_id is required when no Authorization header provided");
                }
                customer_identifier = body["customer_id"].s();
            }

            DBBookingService event_service(db.get(), std::chrono::minutes(10), event_id);
            const auto cancel_result = event_service.cancel_booking(seat_id, customer_identifier);
            bool cancelled = cancel_result.success;
            std::optional<std::string> offer_token = cancel_result.offer_token;
            crow::json::wvalue response;
            response["success"] = cancelled;
            response["message"] = cancelled ? "Booking cancelled" : "Booking not found";
            if (offer_token.has_value())
            {
                response["offer_token"] = *offer_token;
            }
            return crow::response(cancelled ? 200 : 404, response);
        });

    const char *port_value = std::getenv("PORT");
    const auto port = port_value == nullptr ? 8080 : std::stoi(port_value);
    std::cout << "Ticket Booking System listening on port " << port << "\n";
    
    std::atomic<bool> running{true};
    std::thread maintenance_thread([&db, &running]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (db.get()) {
                const char *ttl_env = std::getenv("HOLD_TTL_SECONDS");
                const int ttl = ttl_env ? std::stoi(ttl_env) : 600;
                DBBookingService maintenance_service(db.get(), std::chrono::seconds(ttl), 0);
                maintenance_service.run_maintenance();
            }
        }
    });

    app.port(port).multithreaded().run();
    running = false;
    if (maintenance_thread.joinable()) {
        maintenance_thread.join();
    }
}
