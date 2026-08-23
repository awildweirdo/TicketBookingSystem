# Ticket Booking System

A movie/concert ticket booking platform built in C++ using the Crow web framework and SQLite.

## Features

- **RBAC**: Admin (venue creation), Organiser (event creation, analytics), Customer (booking, waitlist).
- **Concurrent Bookings**: Optimistic concurrency with SQLite prevents double-bookings.
- **Seat Hold TTL**: Configurable TTL for seat holds, auto-releasing on timeout.
- **Waitlist Cascade**: Time-limited offers automatically cascade to the next user when they expire or when a booking is canceled.
- **QR Codes**: Encoded booking references sent via SendGrid emails.
- **REST APIs**: Full CRUD operations for venues, events, and bookings.

## Setup Instructions

### Environment Variables
Create a `.env` file based on `.env.example`:
```env
JWT_SECRET=your_super_secret_key
JWT_TTL=3600
PORT=8080
SENDGRID_API_KEY=SG.your_key_here
SENDGRID_FROM_EMAIL=tickets@example.com
APP_BASE_URL=http://localhost:8080
HOLD_TTL_SECONDS=600
DB_PATH=booking.db
```

### Docker
```bash
# Build the image
docker buildx build -t ticket-booking .

# Run the container (persisting data)
docker run -p 8080:8080 -v "$(pwd)/data":/app/data --env-file .env ticket-booking
```

### Local Build (Linux)
```bash
sudo apt-get install build-essential cmake libsqlite3-dev
cmake -S . -B build
cmake --build build
DB_PATH=booking.db ./build/ticket_booking_app
```

## API Documentation

- `POST /api/register` - Register a new user (`email`, `password`, `role`).
- `POST /api/login` - Login (`email`, `password`) -> Returns JWT.
- `POST /api/venues` (Admin) - Create venue with `name` and `seats` array.
- `GET /api/venues` - List all venues.
- `POST /api/events` (Organiser) - Create event.
- `GET /api/events` - List all events.
- `GET /api/events/:id/seats` - Get all seats for an event.
- `POST /api/events/:id/seats/:seatId/hold` (Customer) - Hold a seat.
- `POST /api/events/:id/seats/:seatId/confirm` (Customer) - Confirm a held seat.
- `POST /api/events/:id/seats/:seatId/cancel` (Customer) - Cancel a booking.
- `POST /api/events/:id/waitlist` (Customer) - Join waitlist for a sold-out category.
- `POST /api/events/:id/waitlist/:token/accept` (Customer) - Accept waitlist offer.

For more architectural details, see `SYSTEM_DESIGN.md`.
