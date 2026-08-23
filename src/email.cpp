#include "email.h"

#include <filesystem>
#include <fstream>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstdlib>

namespace
{
    std::string url_encode(const std::string &value)
    {
        std::ostringstream encoded;
        encoded.fill('0');
        encoded << std::hex;
        for (unsigned char c : value)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                encoded << c;
            }
            else
            {
                encoded << '%' << std::uppercase << std::setw(2) << static_cast<int>(c) << std::nouppercase;
            }
        }
        return encoded.str();
    }

    std::string json_escape(const std::string &value)
    {
        std::ostringstream out;
        for (char c : value)
        {
            switch (c)
            {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
            }
        }
        return out.str();
    }
}

std::string build_qr_code_url(const std::string &booking_reference)
{
    return "https://api.qrserver.com/v1/create-qr-code/?size=220x220&data=" + url_encode(booking_reference);
}

std::string build_ticket_email_html(const std::string &booking_reference,
                                    const std::string &event_title,
                                    const std::string &seat_label,
                                    const std::string &qr_code_url)
{
    std::ostringstream html;
    html << "<html><body style=\"font-family:Arial,sans-serif;line-height:1.5;\">"
         << "<h2>Your ticket is confirmed</h2>"
         << "<p><strong>Booking reference:</strong> " << booking_reference << "</p>"
         << "<p><strong>Event:</strong> " << event_title << "</p>"
         << "<p><strong>Seat:</strong> " << seat_label << "</p>"
         << "<p>Scan the QR code below at entry.</p>"
         << "<p><img src=\"" << qr_code_url << "\" alt=\"QR code ticket\" /></p>"
         << "</body></html>";
    return html.str();
}

std::string build_waitlist_offer_email_html(const std::string &accept_url,
                                            const std::string &category,
                                            const std::string &event_title)
{
    std::ostringstream html;
    html << "<html><body style=\"font-family:Arial,sans-serif;line-height:1.5;\">"
         << "<h2>A seat is available for you</h2>"
         << "<p>Your waitlist request for <strong>" << category << "</strong> on <strong>" << event_title << "</strong> is ready.</p>"
         << "<p>Complete your booking before the offer expires:</p>"
         << "<p><a href=\"" << accept_url << "\">Complete booking now</a></p>"
         << "</body></html>";
    return html.str();
}

EmailSendResult send_sendgrid_email(const std::string &api_key,
                                    const std::string &from_email,
                                    const std::string &to_email,
                                    const std::string &subject,
                                    const std::string &html_body,
                                    const std::string &text_body)
{
    if (api_key.empty())
    {
        std::cout << "[email] SENDGRID_API_KEY not set, skipping email to " << to_email << "\n";
        return {true, "email skipped (no API key)"};
    }

    const auto payload_path = std::filesystem::temp_directory_path() / ("ticket_email_" + std::to_string(std::rand()) + ".json");
    std::ofstream payload(payload_path);
    if (!payload)
    {
        return {false, "failed to create temp payload"};
    }

    payload << "{"
            << "\"personalizations\":[{\"to\":[{\"email\":\"" << json_escape(to_email) << "\"}]}],"
            << "\"from\":{\"email\":\"" << json_escape(from_email) << "\"},"
            << "\"subject\":\"" << json_escape(subject) << "\","
            << "\"content\":["
            << "{\"type\":\"text/plain\",\"value\":\"" << json_escape(text_body) << "\"},"
            << "{\"type\":\"text/html\",\"value\":\"" << json_escape(html_body) << "\"}]"
            << "}";
    payload.close();

    const std::string command = "curl -sS -o /dev/null -X POST https://api.sendgrid.com/v3/mail/send "
                                "-H \"Authorization: Bearer " + api_key + "\" "
                                "-H \"Content-Type: application/json\" "
                                "--data-binary @\"" + payload_path.string() + "\"";

    const int rc = std::system(command.c_str());
    std::filesystem::remove(payload_path);
    if (rc != 0)
    {
        return {false, "sendgrid curl command failed"};
    }
    return {true, "email queued"};
}
