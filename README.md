# Desk display

A cycling desk screen for the LilyGO T-Display-S3. It shows a clock, local weather, Bitcoin and Lightning figures, a hashrate chart, a price chart, posts from a public search, and a few server checks. The pages, order, time zone, ZIP code, charts, and extra servers are edited in a small web page on the board. Saving does not require a new firmware flash.

The board is a LilyGO / TTGO T-Display-S3: ESP32-S3, 16 MB flash, 8 MB PSRAM, and a 1.9 inch 170x320 ST7789. The firmware runs it in landscape, 320 by 170. The two buttons on GPIO 0 and GPIO 14 move between pages.

## Firmware

Install [PlatformIO](https://platformio.org/), then from this directory:

```
py -m platformio run -e lilygo-t-display-s3 -t upload
```

Copy `include/secrets.h.example` to `include/secrets.h` first. That file holds the Wi-Fi name and password, the address of the status bridge, and the portal login. It is not committed.

Copy `include/local.h.example` to `include/local.h` when you want a home ZIP code or addresses for a Pi and a StartOS machine baked in as defaults. The portal can change the ZIP later. `local.h` is not committed.

`monitor_rts` and `monitor_dtr` are off so opening the USB serial port does not reset the board. PlatformIO picks the serial port. Pass `--upload-port` if more than one board is connected.

After the board joins Wi-Fi, open `http://desk-display.local/` or the board's IP address. The login is the user and password from `secrets.h`. The same login is used for **Firmware update**. Later firmware can be uploaded there while the USB cable is disconnected from a serial monitor. Leave the cable connected only for power if you like.

If the Wi-Fi name in `secrets.h` is not found, the board opens an access point named `DeskDisplay`. Join it and open `http://192.168.4.1/`.

## Status bridge

The bridge is a Python 3 program that collects Bitcoin figures, the price history, and optional SSH checks, then serves one JSON document to the board. Public numbers come from mempool.space. Posts come from a public Nitter search. The bridge does not write to those machines.

```
copy bridge\bridge_config.json.example bridge\bridge_config.json
py bridge\status_bridge.py
```

`bridge_config.json` is not committed. Put the same token there that you put in `BRIDGE_TOKEN`. Generate at least 32 random hex characters. Set `bitcoin_cli` and `bitcoin_dir` if a local Bitcoin Core should be preferred over mempool.space. Set `pi_host`, `pi_user`, and `pi_key` to read load, memory, disk, and the Jellyfin, Vaultwarden, and Frigate port checks over SSH. Set the `startos_*` fields to read a StartOS machine and, when it is up, Core Lightning or LND balances.

`bridge/start-bridge.ps1` restarts the bridge until it exits cleanly. A port already in use counts as clean, so a second copy stops.

The board polls `http://BRIDGE_HOST:8765/status` with the header `X-Desk-Key`. `/health` needs no key.

## What the portal edits

The portal saves on the board, in its own flash, and applies on the next refresh:

- which pages are on, and their order
- seconds on each page
- time zone
- US ZIP code for the weather page
- hashrate window: 3 days, 7 days, 30 days, 3 months, or 1 year
- price window: 24 hours, 7 days, 30 days, 3 months, or 1 year
- price currency: USD, EUR, GBP, CAD, CHF, AUD, or JPY
- post search word
- up to four extra servers, each with a name, address, and TCP port
- a note title and body, shown only while the Note page is checked

The Bitcoin page keeps the US dollar price. The Price page uses the currency you pick.

## Pages

Clock, weather, Bitcoin, network, hashrate, price, Lightning, servers, one post per screen, and the optional note. Weather icons are drawn on the panel. The hashrate and price charts are scaled to the numbers in the window you picked.

## License

MIT. See [LICENSE](LICENSE).
