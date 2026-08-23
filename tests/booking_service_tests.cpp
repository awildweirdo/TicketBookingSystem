#include "booking_service.h"

#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

int main()
{
    BookingService service(std::chrono::milliseconds(20));
    service.add_seat("A1", "Standard");

    const auto first_hold = service.hold_seat("A1", "customer-1");
    const auto second_hold = service.hold_seat("A1", "customer-2");
    assert(first_hold.success);
    assert(!second_hold.success);

    assert(!service.confirm_seat("A1", "customer-2").success);
    assert(service.confirm_seat("A1", "customer-1").success);
    assert(!service.cancel_booking("A1", "customer-2"));
    assert(service.cancel_booking("A1", "customer-1"));

    assert(service.hold_seat("A1", "customer-3").success);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(service.release_expired_holds() == 1);
    assert(service.hold_seat("A1", "customer-4").success);

    BookingService concurrent_service(std::chrono::seconds(1));
    concurrent_service.add_seat("B1", "Premium");
    std::vector<bool> results(8, false);
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < results.size(); ++index)
    {
        workers.emplace_back([&concurrent_service, &results, index]()
                             { results[index] = concurrent_service.hold_seat("B1", "customer-" + std::to_string(index)).success; });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    std::size_t successful_holds = 0;
    for (const bool result : results)
    {
        successful_holds += result ? 1 : 0;
    }
    assert(successful_holds == 1);
    return 0;
}
