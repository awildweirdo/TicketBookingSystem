# 🎫 Ticket Booking System

A full-stack movie/concert ticket booking platform built in **C++17** using the **Crow** web framework and **SQLite**. Features real-time seat maps, event poster support, role-based access control, waitlist cascading, and email confirmations with QR codes.

**🔗 Live Demo:** [https://ticketbookingsystem-h2to.onrender.com/](https://ticketbookingsystem-h2to.onrender.com/)

> ⚠️ **Note:** Booking confirmation and waitlist offer emails may land in your **spam/junk folder**. This is because the sender email domain has not been authenticated with SendGrid (SPF/DKIM), causing Gmail and other providers to flag the emails.

---

## Features

### Core Booking
- **Interactive Seat Map** — Real-time visual grid with color-coded status (available / held / booked). Seats update every 5 seconds via polling.
- **Seat Hold with TTL** — Configurable time-to-live for seat holds (default 10 minutes). Background thread auto-releases expired holds.
- **Booking Confirmation** — Hold → Confirm two-step flow with atomic database transactions preventing double-bookings.
- **Booking Cancellation** — Customers can cancel confirmed bookings, triggering waitlist cascade.

### Venue & Event Management
- **Visual Seat Builder** — Admin drag-and-click grid builder for venue layouts. Supports Standard, Premium, VIP, and Disabled seat categories.
- **Disabled Seat Gaps** — Disabled seats maintain their grid positions, preserving venue layout with visible gaps.
- **Event Creation** — Organisers create events tied to venues with category-based pricing.
- **Venue Double-Booking Prevention** — Events at the same venue within 3 hours are rejected.

### Waitlist System
- **FIFO Waitlist Queue** — Per-category waitlist for sold-out events.
- **Time-Limited Offers** — When a seat opens (cancellation/hold expiry), the next waitlisted user receives a secure token-based email offer.
- **Auto-Cascade** — If an offer expires without acceptance, it automatically cascades to the next user in the queue.
- **Tamper-Proof Tokens** — Offer tokens are hashed (SHA-256) before storage; raw tokens are emailed.

### Authentication & Authorization
- **JWT Authentication** — Hand-rolled HS256 JWT implementation using OpenSSL (no third-party JWT library).
- **Salted Password Hashing** — SHA-256 with random 16-byte salt per user.
- **Role-Based Access Control (RBAC):**
  - **Admin** — Venue creation, event deletion, full analytics.
  - **Organiser** — Event creation, per-event revenue/seat analytics.
  - **Customer** — Browsing, booking, waitlist, booking management.

### Email & Notifications
- **SendGrid Integration** — Transactional emails for booking confirmations and waitlist offers.
- **QR Code Tickets** — Booking reference encoded as a QR code (via qrserver.com API) embedded in confirmation emails.

### Concurrency & Data Integrity
- **Optimistic Concurrency** — SQLite `BEGIN IMMEDIATE` transactions with conditional atomic UPDATEs prevent race conditions.
- **WAL Mode** — Write-Ahead Logging for better concurrent read performance.
- **Background Maintenance** — Dedicated thread runs every 5 seconds to expire holds and cascade waitlist offers.

---

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Language | C++17 |
| Web Framework | [Crow](https://crowcpp.org/) v1.2.1 |
| Database | SQLite3 (with WAL mode) |
| Auth | Custom JWT (HS256) via OpenSSL |
| Email | SendGrid REST API (via curl) |
| Build System | CMake 3.20+ with FetchContent |
| Containerization | Docker (multi-stage Debian Bookworm) |
| Deployment | Render.com (Docker) |
| Frontend | Vanilla HTML/CSS/JS (single-page app) |

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                Frontend (SPA)                    │
│         public/index.html                        │
│    Hash-based routing · Fetch API · Polling      │
└──────────────────────┬──────────────────────────┘
                       │ REST API (JSON)
┌──────────────────────▼──────────────────────────┐
│              Crow HTTP Server                    │
│           src/main.cpp (routes)                  │
│                                                  │
│  ┌──────────┐  ┌──────────────┐  ┌───────────┐  │
│  │ auth.cpp │  │ db_booking_  │  │ email.cpp │  │
│  │ JWT/Hash │  │ service.cpp  │  │ SendGrid  │  │
│  └──────────┘  └──────┬───────┘  └───────────┘  │
│                       │                          │
│              ┌────────▼────────┐                 │
│              │    db.cpp       │                 │
│              │  SQLite Wrapper │                 │
│              └────────┬────────┘                 │
└───────────────────────┼─────────────────────────┘
                ┌───────▼───────┐
                │  SQLite DB    │
                │ booking.db    │
                └───────────────┘
```

---

## Setup Instructions

### Prerequisites
- C++17 compiler (GCC 8+ or Clang 7+)
- CMake 3.20+
- SQLite3 development libraries
- OpenSSL development libraries
- Boost (system, date_time) — required by Crow
- curl — for SendGrid email delivery

### Environment Variables

Create a `.env` file based on `.env.example`:
```env
JWT_SECRET=your_super_secret_key
JWT_TTL=3600
PORT=8080
SENDGRID_API_KEY=SG.your_key_here
SENDGRID_FROM_EMAIL=tickets@yourdomain.com
APP_BASE_URL=http://localhost:8080
HOLD_TTL_SECONDS=600
DB_PATH=booking.db
```

### Docker (Recommended)
```bash
# Build the image
docker build -t ticket-booking .

# Run the container (persisting data)
docker run -p 8080:8080 -v $(pwd)/data:/app/data --env-file .env ticket-booking
```

### Local Build (Linux)
**1. Install system dependencies:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git \
    libsqlite3-dev libssl-dev \
    libboost-system-dev libboost-date-time-dev \
    ca-certificates
```

**2. Configure and build** (CMake automatically downloads the Crow framework via `FetchContent` requires internet access at build time):
```bash
rm -rf build
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build -j$(nproc)
```

**3. First run fresh database:**

If upgrading from an older version (without `poster_url`), delete the old database first so the new schema is applied:
```bash
rm -f booking.db data/booking.db
```

Then run from the **workspace root** (so the app can find `public/`, `database/`, and `posters/`):
```bash
./build/ticket_booking_app
```

Or with a custom DB path and port:
```bash
DB_PATH=./data/booking.db PORT=8080 ./build/ticket_booking_app
```

**4. Open in browser:** [http://localhost:8080](http://localhost:8080)

---

### Default Accounts
On first run, the system seeds:
- **Admin:** `admin@admin.com` / `password123`
- **Demo Events:** Coldplay Live (concert) and Dune: Part Two (movie) with a 6×10 seat grid.

---

## API Documentation

### Authentication
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `POST` | `/api/register` | None | Register user. Body: `{ email, password, role }` |
| `POST` | `/api/login` | None | Login. Body: `{ email, password }` → Returns JWT |

### Venues (Admin)
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `POST` | `/api/venues` | Admin | Create venue with seat layout. Body: `{ name, seats: [{ seat_label, category, row_number, column_number }] }` |
| `GET` | `/api/venues` | None | List all venues |

### Events
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `POST` | `/api/events` | Organiser/Admin | Create event. Body: `{ venue_id, title, event_type, starts_at, prices: [{ category, price_cents }] }` |
| `GET` | `/api/events` | None | List all events |
| `GET` | `/api/events/:id/summary` | Organiser/Admin | Revenue and seat count analytics |

### Seat Operations (Customer)
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `GET` | `/api/events/:id/seats` | None | Get seat map with statuses and prices |
| `POST` | `/api/events/:id/seats/:seatId/hold` | Customer | Hold a seat (10-min TTL) |
| `POST` | `/api/events/:id/seats/:seatId/confirm` | Customer | Confirm a held seat |
| `POST` | `/api/events/:id/seats/:seatId/cancel` | Customer | Cancel a booking |

### Waitlist (Customer)
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `POST` | `/api/events/:id/waitlist` | Customer | Join waitlist for sold-out category. Body: `{ category }` |
| `POST` | `/api/events/:id/waitlist/:token/accept` | Customer | Accept a waitlist offer |

### Bookings (Customer)
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `GET` | `/api/me/bookings` | Customer | List user's bookings |
| `POST` | `/api/me/bookings/:id/cancel` | Customer | Cancel a booking by ID |

### Utility
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `GET` | `/api/health` | None | Health check |

---

## Database Schema

```
users ─────────────┐
  id, email,       │
  password_hash,   ├──→ events ──────────────→ event_seats
  role             │      id, title,             id, seat_label,
                   │      venue_id,              category, status,
venues ────────────┤      event_type,            held_by → users
  id, name         │      starts_at
                   │          │
venue_seats ───────┘          ├──→ event_prices (category, price_cents)
  seat_label,                 │
  row_number,                 ├──→ bookings ──→ booking_seats
  column_number               │      booking_reference,
                              │      total_cents, status
                              │
                              └──→ waitlist_entries ──→ waitlist_offers
                                     category, status      token_hash,
                                                           expires_at
```

10 tables total: `users`, `venues`, `venue_seats`, `events`, `event_prices`, `event_seats`, `bookings`, `booking_seats`, `waitlist_entries`, `waitlist_offers`.

---

## Testing

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DUSE_CROW=OFF
cmake --build build
cd build && ctest --output-on-failure
```

4 test suites:
- `booking_service_tests` — In-memory seat hold/confirm/cancel logic
- `auth_tests` — Password hashing and JWT issue/verify
- `db_booking_service_tests` — Full SQLite-backed booking flows
- `email_tests` — Email HTML generation and QR URL building

---

## System Design

For detailed architectural decisions on concurrency, waitlist cascading, and time-limited offers, see [`SYSTEM_DESIGN.md`](SYSTEM_DESIGN.md).

---

## Project Structure

```
├── CMakeLists.txt              # Build configuration
├── Dockerfile                  # Multi-stage Docker build
├── database/
│   └── schema.sql              # SQLite schema (10 tables)
├── include/
│   └── booking_service.h       # Seat struct, BookingService class
├── src/
│   ├── main.cpp                # Crow routes, seed data, maintenance thread
│   ├── db.cpp / db.h           # SQLite RAII wrapper
│   ├── db_booking_service.cpp/h # DB-backed booking, waitlist, cascade logic
│   ├── booking_service.cpp     # In-memory booking service
│   ├── auth.cpp / auth.h       # JWT + password hashing (OpenSSL)
│   └── email.cpp / email.h     # SendGrid email sending
├── public/
│   └── index.html              # Frontend SPA (HTML/CSS/JS)
├── tests/                      # Unit tests (4 suites)
├── SYSTEM_DESIGN.md            # Architectural decisions
└── .env.example                # Environment variable template
```
