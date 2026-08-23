#include "booking_service.h"

#include <utility>

BookingService::BookingService(std::chrono::steady_clock::duration hold_ttl)
    : hold_ttl_(hold_ttl) {}

void BookingService::add_seat(std::string seat_id, std::string category)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Seat s;
    s.id = seat_id;
    s.category = category;
    seats_.emplace(s.id, std::move(s));
}

HoldResult BookingService::hold_seat(const std::string &seat_id, const std::string &customer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    release_expired_holds_locked();

    auto seat = seats_.find(seat_id);
    if (seat == seats_.end())
    {
        return {false, "Seat does not exist"};
    }
    if (seat->second.status != SeatStatus::Available)
    {
        return {false, "Seat is not available"};
    }

    seat->second.status = SeatStatus::Held;
    seat->second.holder = customer_id;
    seat->second.hold_expires_at = std::chrono::steady_clock::now() + hold_ttl_;
    return {true, "Seat held"};
}

HoldResult BookingService::confirm_seat(const std::string &seat_id, const std::string &customer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    release_expired_holds_locked();

    auto seat = seats_.find(seat_id);
    if (seat == seats_.end())
    {
        return {false, "Seat does not exist"};
    }
    if (seat->second.status != SeatStatus::Held || seat->second.holder != customer_id)
    {
        return {false, "Seat is not held by this customer"};
    }

    seat->second.status = SeatStatus::Booked;
    seat->second.hold_expires_at = {};
    return {true, "Booking confirmed"};
}

bool BookingService::cancel_booking(const std::string &seat_id, const std::string &customer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto seat = seats_.find(seat_id);
    if (seat == seats_.end() || seat->second.status != SeatStatus::Booked || seat->second.holder != customer_id)
    {
        return false;
    }

    seat->second.status = SeatStatus::Available;
    seat->second.holder.clear();
    return true;
}

std::size_t BookingService::release_expired_holds()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::size_t released = 0;
    for (auto &[id, seat] : seats_)
    {
        if (seat.status == SeatStatus::Held && seat.hold_expires_at <= now)
        {
            seat.status = SeatStatus::Available;
            seat.holder.clear();
            seat.hold_expires_at = {};
            ++released;
        }
    }
    return released;
}

void BookingService::release_expired_holds_locked()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto &[id, seat] : seats_)
    {
        if (seat.status == SeatStatus::Held && seat.hold_expires_at <= now)
        {
            seat.status = SeatStatus::Available;
            seat.holder.clear();
            seat.hold_expires_at = {};
        }
    }
}

std::vector<Seat> BookingService::seats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Seat> result;
    result.reserve(seats_.size());
    for (const auto &[id, seat] : seats_)
    {
        result.push_back(seat);
    }
    return result;
}
