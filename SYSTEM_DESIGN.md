# System Design

This document outlines the architectural decisions for the Ticket Booking System.

## Seat Hold TTL & Auto-Release
When a customer selects a seat, the system issues a temporary hold, governed by a configurable `HOLD_TTL_SECONDS` (defaulting to 10 minutes). The `event_seats` table updates the `status` to `held` and records the `hold_expires_at` timestamp.
A background scheduler task continuously sweeps the database for expired holds (`hold_expires_at < NOW()`). If a hold expires without confirmation, the scheduler automatically reverts the seat status back to `available` and clears the holder information, ensuring abandoned checkouts don't indefinitely block seat availability.

## Concurrency on Simultaneous Seat Selection
To prevent double-booking or race conditions, seat holds and confirmations leverage SQLite's `BEGIN IMMEDIATE` transactions combined with atomic conditional updates (`UPDATE event_seats SET status = 'held' WHERE status = 'available' AND seat_id = ?`). This optimistic concurrency model ensures that if two users attempt to hold or book the same seat simultaneously, only one transaction will successfully update the row, while the other receives an availability error.

## Waitlist Auto-Assignment and Time-Limited Offer Cascade
For sold-out events, users can join a waitlist for a specific category. The waitlist is modeled as a FIFO queue per event category in the `waitlist_entries` table.
If a confirmed booking is canceled or a hold expires for a previously sold-out category, the system triggers the waitlist cascade. It selects the oldest waiting entry and issues a time-limited offer (token-based). The token is hashed and stored in `waitlist_offers` with an `expires_at` timestamp.
The user receives an email with an offer link. If the offer expires without acceptance, the background scheduler marks the offer as expired and automatically cascades the offer to the next user in the waitlist queue for that category, maintaining fairness and maximizing capacity utilization.

## Time-Limited Offers
Time-limited offers are secured by generating a secure random token, sending the raw token to the user via email, and storing only its SHA-256 hash in the database. When the user accepts the offer, the provided token is hashed and verified against the database record, ensuring the link is tamper-proof and secure from leakage via database reads.
