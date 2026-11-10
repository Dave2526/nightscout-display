# Nightscout-Display (LilyGO T-Display-S3)

Zeigt BZ (7-Segment-Optik, Farbe nach Bereich), Trendpfeil, Delta, IOB/COB und den Verlauf.
Werte kommen per **HTTPS aus Nightscout** (Token mit Rolle `readable`), daher in jedem WLAN nutzbar.
**MQTT** dient nur zur Steuerung und zum Status; ohne Broker läuft die Anzeige normal weiter.

## Bauen / Aufspielen
- erstes Mal per USB, z.B. https://espressif.github.io/esptool-js/ (kein Port: BOOT halten, RST drücken)

## Einrichtung
- bis zu **3 WLANs**, der Reihe nach probiert; findet es keins, öffnet sich der AP `NS-Display`
  (192.168.4.1): WLAN, Nightscout-URL, Token. Das neue WLAN kommt vorne dazu, bei 3 belegten fällt das älteste raus.
- Weboberfläche `http://<hostname>/`: WLANs und gespeicherte **Standardwerte** (Helligkeit, Nachtmodus,
  Verlauf, MQTT, Hostname, NTP, Zeitzone). Speichern startet neu.

## MQTT (Präfix je Gerät, z.B. `/raum1/display`, `/raum2/display`)
Status, alles retained, je Wert ein Topic:

| Topic | Inhalt |
|---|---|
| `<präfix>/status` | `online`/`offline` (LWT) |
| `<präfix>/sgv`, `/delta`, `/direction`, `/age_min` | BZ (mg/dl), Änderung, Trend, Alter in Minuten |
| `<präfix>/iob`, `/cob` | aus Nightscout |
| `<präfix>/error` | leer oder Fehlertext (`Token?`, `Verbindung` …) |
| `<präfix>/power`, `/brightness`, `/night`, `/night_active` | `ON`/`OFF`, 0–255, `ON`/`OFF`/`AUTO`, `ON`/`OFF` |
| `<präfix>/ip`, `/ssid`, `/rssi`, `/version` | Gerät |

Befehle:

| Topic | Wert |
|---|---|
| `<präfix>/set/brightness` | `0`–`255` oder `default` |
| `<präfix>/set/power` | `ON`/`OFF` |
| `<präfix>/set/night` | `ON`/`OFF`/`AUTO` |
| `<präfix>/set/refresh` | sofort abrufen |
| `<präfix>/set/reset` | zurück auf Standardwerte |

Befehle gelten nur bis zum Ausstecken (danach Standardwerte). **Nicht retained senden** –
retained Befehle werden ignoriert.

## Tasten
- links kurz: Helligkeit (Stufen, bzw. einschalten) · lang: Bild drehen
- rechts kurz: sofort abrufen · 5 s: WLAN/Token zurücksetzen
