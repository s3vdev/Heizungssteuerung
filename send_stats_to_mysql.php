<?php
/**
 * Script zum Senden der heutigen Statistiken an MySQL-API
 * Verwendung: php send_stats_to_mysql.php
 */

$apiUrl = "http://192.168.1.35/heizungssteuerung/mysql_api.php/stats/daily";

// Aktuelles Datum im Format YYYY-MM-DD
$dateKey = date("Y-m-d");

// Werte aus der Heizungsperiode (30.12.2024, 19:30 - 23:02, 3h 31m)
$data = [
    "date_key" => "2024-12-30",  // Oder $dateKey für heute
    "switches" => 1,
    "on_seconds" => 12660,  // 3h 31m = 12660 Sekunden
    "off_seconds" => 0,
    "diesel_liters" => 4.59,  // Berechnet: 1.3L/h x 3h 31m
    "avg_vorlauf" => 57.0,
    "avg_ruecklauf" => 34.7,
    "min_vorlauf" => 57.0,
    "max_vorlauf" => 57.0,
    "min_ruecklauf" => 34.7,
    "max_ruecklauf" => 34.7,
    "samples" => 1
];

echo "Sende Daten an MySQL-API...\n";
echo "URL: $apiUrl\n";
echo "Payload:\n";
echo json_encode($data, JSON_PRETTY_PRINT) . "\n\n";

// cURL-Request
$ch = curl_init($apiUrl);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_POST, true);
curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode($data));
curl_setopt($ch, CURLOPT_HTTPHEADER, [
    "Content-Type: application/json"
]);

$response = curl_exec($ch);
$httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
$error = curl_error($ch);
curl_close($ch);

if ($error) {
    echo "❌ cURL Fehler: $error\n";
} else {
    echo "HTTP Code: $httpCode\n";
    echo "Response: $response\n";
    if ($httpCode == 200) {
        echo "✅ Erfolgreich gesendet!\n";
    } else {
        echo "❌ Fehler beim Senden\n";
    }
}

