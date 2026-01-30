# Scripts

## doormon_monitor.py

Discovers the Doormon device via **mDNS** (no slow system DNS), then polls `/status` every second. Reports when the device triggers, when a response takes more than 3 seconds, and lets you reset the triggered state by typing `r` and Enter.

**Setup (once):**

```bash
pip install -r scripts/requirements.txt
```

**Run:**

```bash
python scripts/doormon_monitor.py
```

- **Discovery:** Uses mDNS directly so the device is found quickly (no 5s DNS timeout).
- **Polling:** All requests use the resolved IP, so each poll stays fast (~0.1s).
- **Reset:** Type `r` or `reset` and press Enter to send POST `/reset`.
