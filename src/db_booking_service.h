#pragma once

#include "booking_service.h"
#include <sqlite3.h>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

struct WaitlistJoinResult
{
    bool success{false};
    std::string message;
    int entry_id{0};
};

struct WaitlistOfferResult
{
    bool success{false};
    std::string message;
    std::optional<std::string> offer_token;
};

class DBBookingService
{
public:
    // event_id <= 0 means operate across all events (maintenance only).
    explicit DBBookingService(sqlite3 *db, std::chrono::steady_clock::duration hold_ttl, int event_id = 1);

    void add_seat(const std::string &seat_id, const std::string &category);
    HoldResult hold_seat(const std::string &seat_id, const std::string &customer_id);
    HoldResult confirm_seat(const std::string &seat_id, const std::string &customer_id);
    HoldResult confirm_held_seats(const std::string &customer_id);
    WaitlistOfferResult cancel_booking(const std::string &seat_id, const std::string &customer_id);
    WaitlistOfferResult cancel_booking_by_id(int booking_id, const std::string &customer_id);
    WaitlistJoinResult join_waitlist(const std::string &category, const std::string &customer_id);
    HoldResult accept_waitlist_offer(const std::string &offer_token, const std::string &customer_id);
    std::size_t release_expired_holds();
    std::vector<std::string> expire_waitlist_offers();
    void run_maintenance();
    std::vector<Seat> seats() const;
    int event_id() const { return event_id_; }

private:
    int get_or_create_user(const std::string &customer_id) const;
    std::optional<std::string> offer_next_waitlist_for_category(const std::string &category, int event_seat_id);
    long ttl_seconds() const;
    void mark_related_booking_cancelled(int event_seat_id, int user_id);

    sqlite3 *db_{nullptr};
    std::chrono::steady_clock::duration hold_ttl_{};
    int event_id_{1};
};
