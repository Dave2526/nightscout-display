// Standardwerte für ein frisch geflashtes Gerät. Für eigene Werte als src/config_local.h
// kopieren und anpassen (steht in .gitignore). Alles lässt sich später in der Weboberfläche ändern.
#pragma once
#define DEFAULT_NS_URL "https://nightscout.example.org"
#define DEFAULT_MQTT_HOST ""  // leer = MQTT aus
#define DEFAULT_PREFIX "/display"
// Ziele für den Netztest (<präfix>/set/nettest): Name, IP, Port
#define NETTEST_TARGETS \
  {"nightscout:443", IPAddress(192, 0, 2, 1), 443}, \
  {"nightscout:80", IPAddress(192, 0, 2, 1), 80}, \
  {"1.1.1.1:443", IPAddress(1, 1, 1, 1), 443},
