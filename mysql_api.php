<?php
/**
 * MySQL API für Heizungssteuerung Statistiken
 * 
 * Installation:
 * 1. Diese Datei auf dem NAS ablegen (z.B. /var/services/web/mysql_api/mysql_api.php)
 * 2. MySQL-Verbindungsdaten unten anpassen
 * 3. PHP MySQLi Extension muss aktiviert sein
 */

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

// Handle preflight requests
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

// MySQL-Verbindungsdaten
$mysql_host = 'localhost'; // oder 127.0.0.1
$mysql_user = 'root';
$mysql_password = 'S!HkQbGNS6)J4JA6';
$mysql_database = 'heizungssteuerung';

// Verbindung herstellen
$mysqli = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database);

// Prüfe Verbindung
if ($mysqli->connect_error) {
    http_response_code(500);
    echo json_encode(['error' => 'MySQL connection failed: ' . $mysqli->connect_error]);
    exit;
}

$mysqli->set_charset('utf8mb4');

// API Endpunkte
$method = $_SERVER['REQUEST_METHOD'];

// Get path from PATH_INFO (most reliable - works with mod_rewrite or direct access)
$path = isset($_SERVER['PATH_INFO']) ? $_SERVER['PATH_INFO'] : '';

// Fallback: parse from REQUEST_URI if PATH_INFO not available
if (empty($path)) {
    $request_uri = $_SERVER['REQUEST_URI'];
    $parsed = parse_url($request_uri);
    $full_path = isset($parsed['path']) ? $parsed['path'] : '';
    
    // Get script path (e.g., /heizungssteuerung/mysql_api.php)
    $script_path = $_SERVER['SCRIPT_NAME'];
    
    // Remove script path from full path to get the endpoint path
    if (strpos($full_path, $script_path) === 0) {
        $path = substr($full_path, strlen($script_path));
    } else {
        // Alternative: try to find script name and extract after it
        $script_name = basename($script_path);
        $pos = strpos($full_path, $script_name);
        if ($pos !== false) {
            $path = substr($full_path, $pos + strlen($script_name));
        } else {
            $path = $full_path;
        }
    }
}

// Clean up path: remove leading/trailing slashes and normalize
$path = trim($path, '/');
if (!empty($path)) {
    $path = '/' . $path;
}

try {
    // GET /health - Health Check
    if ($method === 'GET' && ($path === '/health' || $path === '')) {
        echo json_encode([
            'status' => 'ok',
            'mysql_version' => $mysqli->server_info,
            'database' => $mysql_database
        ]);
    }
    // GET /api/mysql/stats/today - Hole heutige Statistiken
    elseif ($method === 'GET' && strpos($path, '/stats/today') !== false) {
        $date = date('Y-m-d');
        $stmt = $mysqli->prepare("SELECT * FROM daily_stats WHERE date_key = ?");
        $stmt->bind_param('s', $date);
        $stmt->execute();
        $result = $stmt->get_result();
        
        if ($row = $result->fetch_assoc()) {
            echo json_encode($row);
        } else {
            http_response_code(404);
            echo json_encode(['error' => 'No data for today']);
        }
        $stmt->close();
    }
    // GET /api/mysql/stats/days?days=14 - Hole Statistiken für mehrere Tage
    elseif ($method === 'GET' && strpos($path, '/stats/days') !== false) {
        $days = isset($_GET['days']) ? (int)$_GET['days'] : 30;
        $days = max(1, min(365, $days)); // Limit between 1 and 365 days
        
        $stmt = $mysqli->prepare("
            SELECT * FROM daily_stats 
            WHERE date_key >= DATE_SUB(CURDATE(), INTERVAL ? DAY)
            ORDER BY date_key DESC
            LIMIT ?
        ");
        $stmt->bind_param('ii', $days, $days);
        $stmt->execute();
        $result = $stmt->get_result();
        
        $data = [];
        while ($row = $result->fetch_assoc()) {
            $data[] = $row;
        }
        echo json_encode($data);
        $stmt->close();
    }
    // POST /api/mysql/stats/daily - Speichere/Update tägliche Statistiken
    elseif ($method === 'POST' && strpos($path, '/stats/daily') !== false) {
        $input = json_decode(file_get_contents('php://input'), true);
        
        if (!$input || !isset($input['date_key'])) {
            http_response_code(400);
            echo json_encode(['error' => 'Missing date_key']);
            exit;
        }
        
        $date_key = $input['date_key'];
        $switches = isset($input['switches']) ? (int)$input['switches'] : 0;
        $on_seconds = isset($input['on_seconds']) ? (int)$input['on_seconds'] : 0;
        $off_seconds = isset($input['off_seconds']) ? (int)$input['off_seconds'] : 0;
        $avg_vorlauf = isset($input['avg_vorlauf']) && $input['avg_vorlauf'] !== null ? (float)$input['avg_vorlauf'] : null;
        $avg_ruecklauf = isset($input['avg_ruecklauf']) && $input['avg_ruecklauf'] !== null ? (float)$input['avg_ruecklauf'] : null;
        $min_vorlauf = isset($input['min_vorlauf']) && $input['min_vorlauf'] !== null ? (float)$input['min_vorlauf'] : null;
        $max_vorlauf = isset($input['max_vorlauf']) && $input['max_vorlauf'] !== null ? (float)$input['max_vorlauf'] : null;
        $min_ruecklauf = isset($input['min_ruecklauf']) && $input['min_ruecklauf'] !== null ? (float)$input['min_ruecklauf'] : null;
        $max_ruecklauf = isset($input['max_ruecklauf']) && $input['max_ruecklauf'] !== null ? (float)$input['max_ruecklauf'] : null;
        $diesel_liters = isset($input['diesel_liters']) ? (float)$input['diesel_liters'] : 0.0;
        $samples = isset($input['samples']) ? (int)$input['samples'] : 0;
        
        $stmt = $mysqli->prepare("
            INSERT INTO daily_stats (
                date_key, switches, on_seconds, off_seconds,
                avg_vorlauf, avg_ruecklauf, min_vorlauf, max_vorlauf,
                min_ruecklauf, max_ruecklauf, diesel_liters, samples
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE
                switches = VALUES(switches),
                on_seconds = VALUES(on_seconds),
                off_seconds = VALUES(off_seconds),
                avg_vorlauf = VALUES(avg_vorlauf),
                avg_ruecklauf = VALUES(avg_ruecklauf),
                min_vorlauf = VALUES(min_vorlauf),
                max_vorlauf = VALUES(max_vorlauf),
                min_ruecklauf = VALUES(min_ruecklauf),
                max_ruecklauf = VALUES(max_ruecklauf),
                diesel_liters = VALUES(diesel_liters),
                samples = VALUES(samples)
        ");
        
        $stmt->bind_param(
            'siiidddddddi',
            $date_key, $switches, $on_seconds, $off_seconds,
            $avg_vorlauf, $avg_ruecklauf, $min_vorlauf, $max_vorlauf,
            $min_ruecklauf, $max_ruecklauf, $diesel_liters, $samples
        );
        
        if ($stmt->execute()) {
            echo json_encode(['success' => true, 'date_key' => $date_key]);
        } else {
            http_response_code(500);
            echo json_encode(['error' => 'Failed to save: ' . $stmt->error]);
        }
        $stmt->close();
    }
    // POST /api/mysql/events - Speichere Switch-Event
    elseif ($method === 'POST' && strpos($path, '/events') !== false) {
        $input = json_decode(file_get_contents('php://input'), true);
        
        if (!$input || !isset($input['timestamp']) || !isset($input['is_on'])) {
            http_response_code(400);
            echo json_encode(['error' => 'Missing required fields']);
            exit;
        }
        
        $timestamp = $input['timestamp'];
        $is_on = (int)$input['is_on'];
        $temp_vorlauf = isset($input['temp_vorlauf']) && $input['temp_vorlauf'] !== null ? (float)$input['temp_vorlauf'] : null;
        $temp_ruecklauf = isset($input['temp_ruecklauf']) && $input['temp_ruecklauf'] !== null ? (float)$input['temp_ruecklauf'] : null;
        $tank_liters = isset($input['tank_liters']) && $input['tank_liters'] !== null ? (float)$input['tank_liters'] : null;
        
        $stmt = $mysqli->prepare("
            INSERT INTO switch_events (timestamp, is_on, temp_vorlauf, temp_ruecklauf, tank_liters)
            VALUES (?, ?, ?, ?, ?)
        ");
        
        $stmt->bind_param('siddd', $timestamp, $is_on, $temp_vorlauf, $temp_ruecklauf, $tank_liters);
        
        if ($stmt->execute()) {
            echo json_encode(['success' => true, 'id' => $mysqli->insert_id]);
        } else {
            http_response_code(500);
            echo json_encode(['error' => 'Failed to save event: ' . $stmt->error]);
        }
        $stmt->close();
    }
    // GET /api/mysql/events/recent?limit=50 - Hole letzte Switch-Events
    elseif ($method === 'GET' && strpos($path, '/events/recent') !== false) {
        $limit = isset($_GET['limit']) ? (int)$_GET['limit'] : 50;
        $limit = max(1, min(200, $limit)); // Limit between 1 and 200
        
        $stmt = $mysqli->prepare("
            SELECT * FROM switch_events
            ORDER BY timestamp DESC
            LIMIT ?
        ");
        $stmt->bind_param('i', $limit);
        $stmt->execute();
        $result = $stmt->get_result();
        
        $data = [];
        while ($row = $result->fetch_assoc()) {
            // Convert MySQL DATETIME to Unix timestamp for compatibility
            $dt = new DateTime($row['timestamp']);
            $row['timestamp_unix'] = $dt->getTimestamp();
            $data[] = $row;
        }
        echo json_encode($data);
        $stmt->close();
    }
    else {
        http_response_code(404);
        echo json_encode(['error' => 'Endpoint not found: ' . $path]);
    }
} catch (Exception $e) {
    http_response_code(500);
    echo json_encode(['error' => $e->getMessage()]);
} finally {
    $mysqli->close();
}
?>
