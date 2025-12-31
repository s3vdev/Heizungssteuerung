#!/bin/bash

# Script zum Senden der heutigen Statistiken an MySQL-API
# Verwendung: ./send_stats_to_mysql.sh

API_URL="http://192.168.1.35/heizungssteuerung/mysql_api.php/stats/daily"

# Aktuelles Datum im Format YYYY-MM-DD
DATE_KEY=$(date +"%Y-%m-%d")

# Werte aus der Heizungsperiode (30.12.2024, 19:30 - 23:02, 3h 31m)
SWITCHES=1
ON_SECONDS=12660  # 3h 31m = 12660 Sekunden
OFF_SECONDS=0
DIESEL_LITERS=4.59  # Berechnet: 1.3L/h x 3h 31m
AVG_VORLAUF=57.0
AVG_RUECKLAUF=34.7
MIN_VORLAUF=57.0
MAX_VORLAUF=57.0
MIN_RUECKLAUF=34.7
MAX_RUECKLAUF=34.7
SAMPLES=1

# JSON-Payload erstellen
JSON_PAYLOAD=$(cat <<EOF
{
  "date_key": "$DATE_KEY",
  "switches": $SWITCHES,
  "on_seconds": $ON_SECONDS,
  "off_seconds": $OFF_SECONDS,
  "diesel_liters": $DIESEL_LITERS,
  "avg_vorlauf": $AVG_VORLAUF,
  "avg_ruecklauf": $AVG_RUECKLAUF,
  "min_vorlauf": $MIN_VORLAUF,
  "max_vorlauf": $MAX_VORLAUF,
  "min_ruecklauf": $MIN_RUECKLAUF,
  "max_ruecklauf": $MAX_RUECKLAUF,
  "samples": $SAMPLES
}
EOF
)

echo "Sende Daten an MySQL-API..."
echo "URL: $API_URL"
echo "Payload:"
echo "$JSON_PAYLOAD" | jq .

# POST-Request senden
curl -X POST "$API_URL" \
  -H "Content-Type: application/json" \
  -d "$JSON_PAYLOAD" \
  -v

echo ""
echo "✅ Fertig!"

