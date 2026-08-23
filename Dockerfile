FROM debian:bookworm AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN cmake -S . -B build -DBUILD_TESTING=OFF \
    && cmake --build build --config Release -j2

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends libsqlite3-0 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --shell /usr/sbin/nologin appuser
WORKDIR /app
COPY --from=build /app/build/ticket_booking_app /app/ticket_booking_app
COPY --from=build /app/public /app/public
COPY --from=build /app/database /app/database

RUN mkdir -p /app/data && chown appuser:appuser /app/data

USER appuser
EXPOSE 8080
ENV PORT=8080
ENV DB_PATH=/app/data/booking.db
VOLUME ["/app/data"]
CMD ["/app/ticket_booking_app"]
