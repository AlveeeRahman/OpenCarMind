# Dashboard

OpenCarMind pairs with [Dashy](https://github.com/Lissy93/dashy), an open-source, self-hosted dashboard. You run it with Docker on your own computer or home server, and it shows your latest drive next to links to every report. Nothing leaves your machine.

## What Dashy does here

Dashy is a start page with widgets, not a charting tool. It cannot plot a CSV file by itself. So the work is split in two:

- `tools/ocm_report.py` reads the sessions from the SD card and writes a small local website: one report per drive with charts of speed, engine speed, coolant and throttle, the model's manoeuvre classes underneath, an index page, and a `summary.json` file. The charts are inline SVG, so a report makes no requests to the internet.
- Dashy reads `summary.json` with its `customapi` widget, embeds the latest report with its `iframe` widget, and links to the index.

## How the data gets there

Dashy runs wherever you start the Docker container. If that is your Mac, the dashboard is local to your Mac. If it is a Raspberry Pi in the hallway, it is local to your home network. It is only on the internet if you expose that machine yourself.

The board does not talk to Dashy. Data travels by SD card:

1. The board saves every session to the card (`/obd/sessions`), flushing at least every 10 seconds.
2. You put the card in your computer. On a Mac it appears as `/Volumes/<card name>`.
3. `ocm_report.py` reads the card and writes the website into `dashboard/site`.
4. A small web server container (nginx) serves that folder, and Dashy reads from it.

To rebuild automatically whenever the card is in, leave `python3 tools/ocm_report.py /Volumes/<card name>/obd dashboard/site --watch 30` running. It rebuilds every 30 seconds while the card is mounted and waits quietly while it is not.

## Set up

You need Docker (with Docker Compose) and Python 3.

```sh
python3 tools/ocm_report.py /Volumes/<card name>/obd dashboard/site
cd dashboard
docker compose up -d
```

Open Dashy at <http://localhost:4000> and the plain reports at <http://localhost:4001>. The Dashy configuration is `dashboard/dashy/conf.yml`. Dashy reads it from `/app/user-data`, which the compose file mounts from `dashboard/dashy`.

Two details to know:

- The widget that embeds the latest drive points at `http://localhost:4001/latest.html`. That works in a browser on the machine running Docker. To open the dashboard from a phone or another computer, replace `localhost` in `conf.yml` with the machine's name or address.
- The `customapi` widget fetches `http://reports/summary.json` through Dashy's own server (`useProxy: true`), using the compose network, so it needs no CORS settings.

If you only want the reports, skip Docker. Open `dashboard/site/index.html` in a browser.

## What is in the report

For each session: start time (or "clock not set" when the board never reached Wi-Fi), rows, minutes, median seconds between rows, maximum speed, an estimated distance (speed integrated over the row gaps), and, when the session has a `.ml.csv` file, the share of time in each manoeuvre class and the mean of the three estimates. Distance is an estimate from the OBD speed signal and is not an odometer reading.

## Status

Checked: `ocm_report.py` on the two replay sessions in `samples/replay` (it runs, the reports render, and no report contains an external address), and the YAML of `docker-compose.yml` and `conf.yml` parses. The widget names and options come from the Dashy documentation. Not checked: the containers themselves. Docker was not available where this was written, so Dashy has not been started with this configuration. The model band was tested on a fixture `.ml.csv` that uses the firmware's column names, not on a file recorded by a board.

## Automatic upload over Wi-Fi

Today there is none, on purpose: the data path is the SD card. A push from the board to a receiver on your home network is possible, but it needs new firmware (a Wi-Fi client, a resume-after-failure queue and a shared secret) and a decision about security on your network. It is not built.
