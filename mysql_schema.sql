-- MySQL Schema für Heizungssteuerung Statistiken
-- Datenbank: heizungssteuerung
-- Host: 192.168.1.35 (DS118Q NAS)
-- User: root
-- Pass: S!HkQbGNS6)J4JA6

-- Tabelle für tägliche Statistiken
CREATE TABLE IF NOT EXISTS `daily_stats` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `date_key` DATE NOT NULL UNIQUE COMMENT 'Datum im Format YYYY-MM-DD',
  `switches` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Anzahl Schaltungen an diesem Tag',
  `on_seconds` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Gesamte ON-Zeit in Sekunden',
  `off_seconds` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Gesamte OFF-Zeit in Sekunden',
  `avg_vorlauf` DECIMAL(4,1) NULL COMMENT 'Durchschnittstemperatur Vorlauf',
  `avg_ruecklauf` DECIMAL(4,1) NULL COMMENT 'Durchschnittstemperatur Rücklauf',
  `min_vorlauf` DECIMAL(4,1) NULL COMMENT 'Minimale Temperatur Vorlauf',
  `max_vorlauf` DECIMAL(4,1) NULL COMMENT 'Maximale Temperatur Vorlauf',
  `min_ruecklauf` DECIMAL(4,1) NULL COMMENT 'Minimale Temperatur Rücklauf',
  `max_ruecklauf` DECIMAL(4,1) NULL COMMENT 'Maximale Temperatur Rücklauf',
  `diesel_liters` DECIMAL(6,2) NOT NULL DEFAULT 0.00 COMMENT 'Dieselverbrauch in Litern',
  `samples` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Anzahl Temperaturmessungen',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX `idx_date` (`date_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Tägliche Statistiken';

-- Tabelle für Switch-Events (Heizungsperioden)
CREATE TABLE IF NOT EXISTS `switch_events` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `timestamp` DATETIME NOT NULL COMMENT 'Zeitpunkt des Events',
  `is_on` TINYINT(1) NOT NULL COMMENT '1 = ON, 0 = OFF',
  `temp_vorlauf` DECIMAL(4,1) NULL COMMENT 'Temperatur Vorlauf beim Event',
  `temp_ruecklauf` DECIMAL(4,1) NULL COMMENT 'Temperatur Rücklauf beim Event',
  `tank_liters` DECIMAL(6,2) NULL COMMENT 'Tankstand in Litern beim Event',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX `idx_timestamp` (`timestamp`),
  INDEX `idx_is_on` (`is_on`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Switch-Events für Heizungsperioden';
