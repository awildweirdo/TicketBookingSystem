#include "email.h"

#include <cassert>
#include <iostream>

int main()
{
    const std::string booking_reference = "BK-123ABC";
    const std::string qr_url = build_qr_code_url(booking_reference);
    assert(qr_url.find("BK-123ABC") != std::string::npos);

    const std::string html = build_ticket_email_html(booking_reference, "Friday Night Movie", "A1", qr_url);
    assert(html.find("BK-123ABC") != std::string::npos);
    assert(html.find("Friday Night Movie") != std::string::npos);
    assert(html.find("A1") != std::string::npos);
    assert(html.find(qr_url) != std::string::npos);

    const auto result = send_sendgrid_email("", "noreply@example.com", "customer@example.com", "Subject", html, "text body");
    assert(result.success);

    std::cout << "email tests passed" << std::endl;
    return 0;
}
