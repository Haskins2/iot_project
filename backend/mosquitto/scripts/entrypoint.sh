#!/bin/sh
# entrypoint.sh — initialise dynsec then hand off to mosquitto
set -e

/mosquitto/scripts/init-dynsec.sh

exec /docker-entrypoint.sh mosquitto -c /mosquitto/config/mosquitto.conf
