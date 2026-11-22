# Rookmachines

Web-app om een of meer zelfgebouwde rookmachines (ESP32 + Bluetooth LE) te bedienen vanaf je telefoon.
Gebaseerd op de ombouw van een JB Systems FX 1700.

## Inhoud

| Map / bestand | Wat |
|---|---|
| `index.html` | De app |
| `manifest.webmanifest`, `sw.js` | Maken de app installeerbaar en laten hem offline openen |
| `fonts/` | Bebas Neue en Oswald (open source, licenties erbij) |
| `icons/` | App-iconen |
| `firmware/rookmachine_esp32.ino` | Code voor de ESP32 in de rookmachine |

## Online zetten met GitHub Pages

1. Maak op github.com een nieuwe repository, bijvoorbeeld `rookmachine`. Zet hem op **Public**.
2. Klik op **Add file → Upload files** en sleep de **inhoud** van deze map erin (dus `index.html` moet in de hoofdmap staan, niet in een submap). Klik op **Commit changes**.
3. Ga naar **Settings → Pages**. Kies bij *Source* **Deploy from a branch**, branch **main**, map **/ (root)**, en klik **Save**.
4. Na een minuut of twee staat de app op `https://<jouw-gebruikersnaam>.github.io/rookmachine/`.

## Installeren op de telefoon

**Android (Chrome)**
1. Open de link in Chrome.
2. Menu (⋮) → **App installeren** of **Toevoegen aan startscherm**.
3. Zet Bluetooth en locatie aan. Druk in de app op **+ Machine** en kies `Rookmachine-XXXX`.

**iPhone**
Safari kan geen Bluetooth gebruiken vanuit een website. Gebruik daarom de gratis app **Bluefy – Web BLE Browser** uit de App Store en open de link daarin. Zonder Bluefy kun je de app op een iPhone wel bekijken en de simulatie gebruiken.

## Een nieuwe versie zetten

1. Upload het nieuwe `index.html` (of andere bestanden) opnieuw via **Add file → Upload files**.
2. Verhoog in `sw.js` de regel `const VERSION = 'rookmachines-v1.0';` (bijvoorbeeld naar `v1.1`), zodat telefoons de nieuwe versie ophalen.

## Firmware op de ESP32 zetten

**Makkelijkste manier (zonder Arduino-programma):** open `https://<jouw-gebruikersnaam>.github.io/rookmachine/installeren.html` in Chrome of Edge op een pc of laptop, sluit de ESP32 met USB aan en klik op **Installeren op ESP32**. De kant-en-klare firmware staat in `firmware/rookmachine_esp32.merged.bin` (gebouwd met ESP32 Arduino core 3.3.13, partition scheme Huge APP).

**Zelf compileren (als je de code aanpast):**

1. Installeer de Arduino IDE 2 en via de Boards Manager **esp32 by Espressif Systems**.
2. Installeer via Library Manager **MAX6675 library** (Adafruit).
3. Open `firmware/rookmachine_esp32.ino`, kies board **ESP32 Dev Module**, partition scheme **Huge APP (3MB No OTA)**, en upload.
4. Controleer in de Seriële Monitor (115200 baud) de temperatuur en de Bluetooth-naam.

Per machine kun je bovenaan de code de grenzen aanpassen, zoals `SP_MAX` (hoogste setpunt). Het setpunt zelf stel je in de app in en wordt in de ESP32 bewaard.

## Pinbezetting (firmware v1.3, print v5.6)

| Functie | GPIO | Op de print |
|---|---|---|
| SSR verwarming (via Q1) | 4 | J6 |
| Pomp-relais (via JP1: direct of via Q2) | 2 | J5 (S + −) |
| Tanksensor | 16 | J3 (JST XH) |
| Groene led "klaar" | 17 | J4 (JST XH) |
| Gele led "opwarmen" | 33 | D3 |
| Rode led "storing" | 32 | D1 |
| Drukknop rook | 13 | J9 |
| MAX6675 CLK / CS / SO | 5 / 18 / 19 | J2 |
| LoRa E220 M0 / M1 / RXD / TXD | 25 / 26 / 27 / 14 | J10 (optioneel) |

## Veiligheid

- De thermische beveiliging (klixon KSD301) blijft altijd in serie met het verwarmingselement.
- De app en de ESP32 laten de pomp alleen lopen als het blok op temperatuur is, met een maximale rooktijd en een stop bij verbindingsverlies. Dat is een extra laag, geen vervanging van de klixon.
