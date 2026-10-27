# HomeMind hardware mobile beta

Flutter/Dart mobile UI and FastAPI adapter for the hardware IDs defined by the
Pi 4 backend.

## Included beta scope

- Controls for the mapped door/window servos, ventilation fan, and indicator LEDs
- Read-only status for microphone, cameras, XY motors, and home sensors
- Pi 4 hardware-map and device-command API boundaries
- QR pairing: scan, validate, user approval, then `POST /api/v1/pairing/claim`
  for a per-phone bearer token (stored with flutter_secure_storage, sent on
  every request; a 401 returns the app to pairing)
- Debug-only demo data using the same Pi 4 hardware IDs

## Project layout

```text
lib/       Flutter mobile frontend
backend/   FastAPI service for Windows development and Pi 4 deployment
android/   Kotlin Android host project
```

## Run the backend on Windows

```powershell
cd backend
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
.\.venv\Scripts\python.exe -m uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

## Run the Flutter app

```powershell
C:\Users\fizli\development\flutter\bin\flutter.bat pub get
C:\Users\fizli\development\flutter\bin\flutter.bat run
```

The default backend URL is `http://10.0.2.2:8000`, which maps from the Android
emulator to Windows localhost. For a physical phone, open Settings -> Device
connection and enter the Windows LAN address or `http://192.168.1.1:8000` when
the API is running directly on Pi 4.

## Beta boundary

Mobile API 0.4 requires a pairing token on every `/api/` call except health
and the claim (device: C++ `native/services/homemind-mobile-api` on
NODE-RPI4-01; PC reference: `backend/`). MQTT commands require the Pi 4 broker
connection; the API returns 503
instead of simulating success when that transport is unavailable. Local
cleartext HTTP is temporarily enabled for lab testing.
