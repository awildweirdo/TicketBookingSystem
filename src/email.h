#pragma once

#include <string>

struct EmailSendResult
{
    bool success{false};
    std::string message;
};

std::string build_qr_code_url(const std::string &booking_reference);
std::string build_ticket_email_html(const std::string &booking_reference,
                                    const std::string &event_title,
                                    const std::string &seat_label,
                                    const std::string &qr_code_url);
std::string build_waitlist_offer_email_html(const std::string &accept_url,
                                            const std::string &category,
                                            const std::string &event_title);
EmailSendResult send_sendgrid_email(const std::string &api_key,
                                    const std::string &from_email,
                                    const std::string &to_email,
                                    const std::string &subject,
                                    const std::string &html_body,
                                    const std::string &text_body);
