FROM node:20-bullseye

ENV NODE_ENV=production
WORKDIR /app

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    build-essential \
    libcairo2-dev \
    libpango1.0-dev \
    libjpeg-dev \
    libgif-dev \
    librsvg2-dev \
    pkg-config \
    ca-certificates \
    curl \
  && rm -rf /var/lib/apt/lists/*

ARG PROCESS_CLI_REF=main
RUN curl -fsSL "https://github.com/aitjcize/esp32-photoframe/archive/refs/heads/${PROCESS_CLI_REF}.tar.gz" \
  | tar -xz --strip-components=2 -C /app "esp32-photoframe-${PROCESS_CLI_REF}/process-cli"

RUN npm ci --omit=dev

COPY run-processing.sh ./run-processing.sh
RUN chmod +x ./run-processing.sh

CMD ["./run-processing.sh"]
