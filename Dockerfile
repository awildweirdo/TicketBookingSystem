FROM debian:bookworm AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates libboost-system-dev libboost-date-time-dev libsqlite3-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN cmake -S . -B build -DBUILD_TESTING=OFF \
    && cmake --build build --config Release -j2 -v

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends libsqlite3-0 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --shell /usr/sbin/nologin appuser
WORKDIR /app
COPY --from=build /app/build/ticket_booking_app /app/ticket_booking_app
COPY --from=build /app/public /app/public
COPY --from=build /app/database /app/database
COPY --from=build /app/posters /app/posters

RUN mkdir -p /app/data && chown appuser:appuser /app/data \
    && chown -R appuser:appuser /app/posters

USER appuser
EXPOSE 8080
ENV PORT=8080
ENV DB_PATH=/app/data/booking.db
VOLUME ["/app/data", "/app/posters"]
CMD ["/app/ticket_booking_app"]
