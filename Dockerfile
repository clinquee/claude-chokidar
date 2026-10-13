# Multi-stage ultra-low compute build
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    make \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN make clean && make -j$(nproc) CXX=g++ all

# Runtime stage: slim container with Chromium
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    chromium \
    ca-certificates \
    procps \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/claude-chokidar /app/claude-chokidar

# Cloud platforms (Render, Koyeb, etc.) assign a PORT environment variable
ENV PORT=8080
EXPOSE 8080

CMD ["/app/claude-chokidar", "--headless", "--interval", "60", "--separate-profile", "--chrome-path", "/usr/bin/chromium"]
