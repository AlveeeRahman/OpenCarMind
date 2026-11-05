# OpenCarMind report: turns the SD card's session files into the HTML report (tools/ocm_report.py) and serves it.
#   docker run --rm -v /path/to/card/obd:/data:ro -p 8080:8080 ghcr.io/alveeerahman/opencarmind-report
# then open http://localhost:8080. Python standard library only; runs as an unprivileged user.
FROM python:3.12-slim@sha256:b18b261e1b4974b4f321451ad5251ded85abb22c2b8b262da527dda1d031ab30

LABEL org.opencontainers.image.source="https://github.com/AlveeeRahman/OpenCarMind" \
      org.opencontainers.image.description="OpenCarMind report: the SD card's drives as an HTML report on port 8080" \
      org.opencontainers.image.licenses="MIT"

COPY tools/ocm_report.py /app/ocm_report.py
RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin report
USER 10001
WORKDIR /tmp
EXPOSE 8080
VOLUME ["/data"]
CMD ["sh", "-c", "python /app/ocm_report.py /data /tmp/site && exec python -m http.server 8080 --directory /tmp/site"]
