# Build stage
FROM node:18-alpine AS builder
WORKDIR /app

# Declare build args — passed in from docker compose or CI
ARG VITE_MQTT_BROKER
ARG VITE_MQTT_USERNAME
ARG VITE_MQTT_PASSWORD

# Make them available to Vite during build
ENV VITE_MQTT_BROKER=$VITE_MQTT_BROKER
ENV VITE_MQTT_USERNAME=$VITE_MQTT_USERNAME
ENV VITE_MQTT_PASSWORD=$VITE_MQTT_PASSWORD

COPY frontend/package*.json ./
RUN npm ci
COPY frontend .
RUN npm run build

# Production stage
FROM node:18-alpine
WORKDIR /app
RUN npm install -g serve
COPY --from=builder /app/dist ./dist
EXPOSE 3000
CMD ["serve", "-s", "dist", "-l", "3000"]