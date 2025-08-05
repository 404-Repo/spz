FROM oven/bun:1.2.19-alpine AS builder

RUN apk update && apk upgrade --no-cache && apk add --no-cache openssl ca-certificates curl unzip && update-ca-certificates

WORKDIR /app
COPY package.json ./
RUN bun install

# Copy the rest of the app
COPY . .
# Fetch latest spz-wasm.zip into public/
RUN curl -sL https://github.com/404-Repo/spz/releases/latest/download/spz-wasm.zip -o spz-wasm.zip \
    && unzip spz-wasm.zip -d public/ \
    && rm spz-wasm.zip

RUN bun run bun:build

FROM oven/bun:1.2.19-alpine AS runner

WORKDIR /app
COPY --from=builder /app .

EXPOSE 3000

CMD ["bun", "run", "bun:start"]