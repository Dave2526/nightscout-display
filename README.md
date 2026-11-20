# Nightscout-Display (LilyGO T-Display-S3)

*[English version](README.en.md)*

Firmware für den [LilyGO T-Display-S3](https://lilygo.cc/products/t-display-s3), die den aktuellen
Blutzucker aus [Nightscout](https://nightscout.github.io/) anzeigt: BZ in 7-Segment-Optik (Farbe nach
Bereich), Trendpfeil, Delta, IOB/COB und den Verlauf.

- Werte kommen per **HTTPS direkt aus Nightscout** (Token mit Rolle `readable`), das Display läuft
  also in jedem WLAN, auch unterwegs.
- **MQTT** ist optional und dient nur zur Steuerung und zum Status. Ohne Broker läuft die Anzeige normal.
- Bis zu **3 WLANs**, Einrichtung über einen eigenen Access Point, Einstellungen über eine Weboberfläche.

> Dieses GitHub-Repo ist ein Spiegel. Entwickelt wird in einem privaten Forgejo, Änderungen kommen
> von dort.

## Bauen und Aufspielen

Gebaut wird mit [PlatformIO](https://platformio.org/) (`platformio.ini` liegt bei).

- Ohne Docker: `NS_OTA_PASS=<passwort> pio run -e tdisplay-s3`
- **Erstes Mal per USB**, z.B. mit dem [esptool-js Web-Flasher](https://espressif.github.io/esptool-js/)
  (`dist/ns-display.bin` an Adresse `0x0`). Wird kein Port gefunden: BOOT halten, RST drücken.

### Eigene Standardwerte

`src/config_example.h` nach `src/config_local.h` kopieren und anpassen: Nightscout-URL, MQTT-Server,
Topic-Präfix und die Ziele des Netztests. `config_local.h` steht in `.gitignore`. Alle Werte lassen
sich später auch in der Weboberfläche ändern.

## Einrichtung

- Findet das Display keines seiner WLANs, öffnet es den Access Point `NS-Display` (192.168.4.1). Dort
  WLAN, Nightscout-URL und Token eintragen. Das neue WLAN kommt vorne dazu, sind alle 3 belegt, fällt
  das älteste raus.
- Weboberfläche `http://<hostname>/`: WLANs, Grenzwerte (Standard aus Nightscout, Knopf „Aus
  Nightscout laden“), Farben je Bereich und die gespeicherten **Standardwerte** (Helligkeit,
  Nachtmodus, Verlauf, MQTT, Hostname, NTP, Zeitzone). Speichern startet das Display neu.

## MQTT (Präfix je Gerät, z.B. `/raum1/display`)

Status, alles retained, je Wert ein Topic:

| Topic | Inhalt |
|---|---|
| `<präfix>/status` | `online`/`offline` (LWT) |
| `<präfix>/sgv`, `/delta`, `/direction`, `/age_min` | BZ (mg/dl), Änderung, Trend, Alter in Minuten |
| `<präfix>/iob`, `/cob` | aus Nightscout |
| `<präfix>/error` | leer oder Fehlertext (`Token?`, `Verbindung` …) |
| `<präfix>/error_detail` | bei `Verbindung`: HTTP-Fehlercode und TLS-Fehler |
| `<präfix>/power`, `/brightness`, `/night`, `/night_active` | `ON`/`OFF`, 0–255, `ON`/`OFF`/`AUTO`, `ON`/`OFF` |
| `<präfix>/ip`, `/ssid`, `/rssi`, `/version` | Gerät |
| `<präfix>/gateway`, `/dns`, `/subnet`, `/bssid`, `/time` | Netz und Uhrzeit (zur Fehlersuche) |
| `<präfix>/nettest` | Ergebnis des Netztests |

Befehle:

| Topic | Wert |
|---|---|
| `<präfix>/set/brightness` | `0`–`255` oder `default` |
| `<präfix>/set/power` | `ON`/`OFF` |
| `<präfix>/set/night` | `ON`/`OFF`/`AUTO` |
| `<präfix>/set/refresh` | sofort abrufen |
| `<präfix>/set/nettest` | TCP-Verbindungstest zu den Zielen aus `config_local.h` |
| `<präfix>/set/reset` | zurück auf Standardwerte |

Befehle gelten nur bis zum Ausstecken (danach Standardwerte). **Nicht retained senden**, retained
Befehle werden ignoriert.

## Tasten

- links kurz: Helligkeit (Stufen bzw. einschalten) · lang: Bild drehen
- rechts kurz: Statusseite (IP, WLAN, MQTT, Nightscout, Laufzeit; nach 30 s zurück) · 5 s halten:
  WLAN/Token zurücksetzen

## Hinweis

Das Display ist eine reine Anzeige und **kein Medizinprodukt**. Therapieentscheidungen nie allein
danach treffen.

## Lizenz

[MIT](LICENSE)

---

Erstellt mit [Claude](https://claude.ai) (Anthropic).
