FROM node:20-bookworm-slim

WORKDIR /app

# Install dependencies and Playwright Chromium with required OS libraries
COPY package*.json ./
RUN npm ci && \
    npx playwright install chromium --with-deps

COPY . .

# Cloud platforms (Render, Koyeb, etc.) assign a PORT environment variable
ENV PORT=8080
ENV NODE_OPTIONS="--max-old-space-size=256"
EXPOSE 8080

CMD ["node", "index.js", "--headless", "--interval", "60", "--separate-profile"]
