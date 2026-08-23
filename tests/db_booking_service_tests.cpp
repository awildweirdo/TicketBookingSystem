#include "db_booking_service.h"
#include "db.h"

#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <iostream>

int main()
{
    // Build schema path from PROJECT_SOURCE_DIR provided by CMake
    std::string schema_path = std::string(PROJECT_SOURCE_DIR) + "/database/schema.sql";
    const std::string db_path = "test_booking.db";

    // Remove existing test DB if present
    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    // Open DB
    DB db(db_path);
    std::string err;
    if (!db.init_schema(schema_path, err))
    {
        std::cerr << "Schema init failed: " << err << std::endl;
        return 1;
    }

    // Seed required FK rows: venue, organiser user, and event
    char *sql_err = nullptr;
    const std::string seed_sql =
        "INSERT INTO venues(name) VALUES('Test Venue');"
        "INSERT INTO users(email, password_hash, role) VALUES('organiser@example.com', '', 'organiser');"
        "INSERT INTO events(organiser_id, venue_id, title, event_type, starts_at) VALUES((SELECT id FROM users WHERE email='organiser@example.com' LIMIT 1),(SELECT id FROM venues WHERE name='Test Venue' LIMIT 1), 'Test Event', 'movie', datetime('now'));";
    if (sqlite3_exec(db.get(), seed_sql.c_str(), nullptr, nullptr, &sql_err) != SQLITE_OK)
    {
        std::cerr << "Seeding DB failed: " << (sql_err ? sql_err : "unknown") << std::endl;
        if (sql_err) sqlite3_free(sql_err);
        return 1;
    }

    DBBookingService svc(db.get(), std::chrono::milliseconds(200));
    svc.add_seat("A1", "Standard");
    svc.add_seat("B1", "Premium");
    svc.add_seat("C1", "Standard");

    const auto r1 = svc.hold_seat("A1", "customer-1");
    const auto r2 = svc.hold_seat("A1", "customer-2");
    assert(r1.success);
    assert(!r2.success);

    assert(!svc.confirm_seat("A1", "customer-2").success);
    assert(svc.confirm_seat("A1", "customer-1").success);
    assert(!svc.cancel_booking("A1", "customer-2").success);
    assert(svc.cancel_booking("A1", "customer-1").success);

    const auto waitlist_join = svc.join_waitlist("Standard", "customer-5");
    assert(waitlist_join.success);

    const auto second_hold = svc.hold_seat("A1", "customer-6");
    assert(second_hold.success);
    assert(svc.confirm_seat("A1", "customer-6").success);
    const auto cancel_with_offer = svc.cancel_booking("A1", "customer-6");
    assert(cancel_with_offer.success);
    assert(cancel_with_offer.offer_token.has_value());

    const auto accept = svc.accept_waitlist_offer(*cancel_with_offer.offer_token, "customer-5");
    if (!accept.success)
    {
        std::cerr << "accept failed: " << accept.message << std::endl;
    }
    assert(accept.success);

    assert(svc.hold_seat("C1", "customer-3").success);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(svc.release_expired_holds() >= 1);
    assert(svc.hold_seat("C1", "customer-4").success);

    // Concurrency test on B1
    std::vector<bool> results(8);
    std::vector<std::thread> workers;
    for (size_t i=0;i<results.size();++i)
    {
        workers.emplace_back([&svc,&results,i]() {
            results[i] = svc.hold_seat("B1", "customer-" + std::to_string(i)).success;
        });
    }
    for (auto &w : workers) w.join();
    size_t succ=0;
    for (auto b: results) if (b) ++succ;
    assert(succ==1);

    // Cleanup
    std::filesystem::remove(db_path, ec);
    return 0;
}
