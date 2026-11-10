# Nightscout-Display (LilyGO T-Display-S3)

Zeigt BZ (7-Segment-Optik, Farbe nach Bereich), Trendpfeil, Delta, IOB/COB und den Verlauf.
Werte kommen per **HTTPS aus Nightscout** (Token mit Rolle `readable`), daher in jedem WLAN nutzbar.
**MQTT** dient nur zur Steuerung und zum Status; ohne Broker läuft die Anzeige normal weiter.

## Bauen / Aufspielen
- erstes Mal per USB, z.B. https://espressif.github.io/esptool-js/ (kein Port: BOOT halten, RST drücken)

## Einrichtung
- ohne WLAN/Token öffnet sich der AP `NS-Display` (192.168.4.1): WLAN, Nightscout-URL, Token
- Weboberfläche `http://<hostname>/`: gespeicherte **Standardwerte** (Helligkeit, Nachtmodus, Verlauf,
  MQTT, Hostname, NTP, Zeitzone). Speichern startet neu.

## MQTT (Präfix je Gerät, z.B. `/raum1/display`, `/raum2/display`)
| Topic | Inhalt |
|---|---|
| `<präfix>/status` | `online`/`offline` (LWT, retained) |
| `<präfix>/state` | JSON: sgv, delta, direction, age_min, iob, cob, power, brightness, night, rssi … |
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
