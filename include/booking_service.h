#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum class SeatStatus
{
    Available,
    Held,
    Booked
};

struct Seat
{
    std::string id;
    std::string category;
    SeatStatus status{SeatStatus::Available};
    std::string holder;
    std::chrono::steady_clock::time_point hold_expires_at{};
    int row_number{0};
    int column_number{0};
    int price_cents{0};
    int held_by_id{0};
    std::string hold_expires_at_iso;
};

struct HoldResult
{
    bool success{false};
    std::string message;
    std::string booking_reference;
    std::string hold_expires_at;
};

class BookingService
{
public:
    explicit BookingService(std::chrono::steady_clock::duration hold_ttl);

    void add_seat(std::string seat_id, std::string category);
    HoldResult hold_seat(const std::string &seat_id, const std::string &customer_id);
    HoldResult confirm_seat(const std::string &seat_id, const std::string &customer_id);
    bool cancel_booking(const std::string &seat_id, const std::string &customer_id);
    std::size_t release_expired_holds();
    std::vector<Seat> seats() const;

private:
    void release_expired_holds_locked();

    std::chrono::steady_clock::duration hold_ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Seat> seats_;
};
