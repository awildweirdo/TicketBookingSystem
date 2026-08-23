PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('customer', 'organiser', 'admin')),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS venues (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS venue_seats (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    venue_id INTEGER NOT NULL REFERENCES venues(id) ON DELETE CASCADE,
    seat_label TEXT NOT NULL,
    category TEXT NOT NULL,
    row_number INTEGER NOT NULL,
    column_number INTEGER NOT NULL,
    UNIQUE (venue_id, seat_label)
);

CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    organiser_id INTEGER NOT NULL REFERENCES users(id),
    venue_id INTEGER NOT NULL REFERENCES venues(id),
    title TEXT NOT NULL,
    event_type TEXT NOT NULL CHECK (event_type IN ('movie', 'concert')),
    starts_at TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS event_prices (
    event_id INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,
    category TEXT NOT NULL,
    price_cents INTEGER NOT NULL CHECK (price_cents >= 0),
    PRIMARY KEY (event_id, category)
);

CREATE TABLE IF NOT EXISTS event_seats (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,
    seat_label TEXT NOT NULL,
    category TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'available' CHECK (status IN ('available', 'held', 'booked')),
    held_by INTEGER REFERENCES users(id),
    hold_expires_at TEXT,
    UNIQUE (event_id, seat_label)
);

CREATE TABLE IF NOT EXISTS bookings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    booking_reference TEXT NOT NULL UNIQUE,
    event_id INTEGER NOT NULL REFERENCES events(id),
    customer_id INTEGER NOT NULL REFERENCES users(id),
    status TEXT NOT NULL DEFAULT 'confirmed' CHECK (status IN ('confirmed', 'cancelled')),
    total_cents INTEGER NOT NULL CHECK (total_cents >= 0),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    cancelled_at TEXT
);

CREATE TABLE IF NOT EXISTS booking_seats (
    booking_id INTEGER NOT NULL REFERENCES bookings(id) ON DELETE CASCADE,
    event_seat_id INTEGER NOT NULL REFERENCES event_seats(id),
    price_cents INTEGER NOT NULL CHECK (price_cents >= 0),
    PRIMARY KEY (booking_id, event_seat_id)
);

CREATE TABLE IF NOT EXISTS waitlist_entries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,
    customer_id INTEGER NOT NULL REFERENCES users(id),
    category TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'waiting' CHECK (status IN ('waiting', 'offered', 'fulfilled', 'expired')),
    seat_count INTEGER NOT NULL DEFAULT 1,
    joined_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS waitlist_offers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    waitlist_entry_id INTEGER NOT NULL REFERENCES waitlist_entries(id) ON DELETE CASCADE,
    event_seat_id INTEGER NOT NULL REFERENCES event_seats(id),
    token_hash TEXT NOT NULL UNIQUE,
    expires_at TEXT NOT NULL,
    completed_at TEXT
);

CREATE INDEX IF NOT EXISTS idx_event_seats_status ON event_seats(event_id, category, status);
CREATE INDEX IF NOT EXISTS idx_waitlist_queue ON waitlist_entries(event_id, category, status, joined_at);
CREATE INDEX IF NOT EXISTS idx_waitlist_offers_expiry ON waitlist_offers(expires_at);
