# Security

## What is protected
| Surface | Protection |
|---|---|
| Dashboard and API (port 8080) | Login (user + password). The password is stored only as a PBKDF2-HMAC-SHA256 hash (60 000 iterations, random salt) in `/root/aid/auth.json` (mode 0600). |
| Sessions | Random 256-bit token in a cookie with `HttpOnly; SameSite=Strict`; ends after 15 min without use (default), 8 h at most; a password change ends all sessions. At most 16 sessions. |
| Brute force | 5 wrong passwords from one address lock that address for 60 s (HTTP 429); failed logins are logged. |
| CSRF | Every POST/DELETE must carry `X-Requested-With: aid-dashboard`; with `SameSite=Strict` cookies a foreign web page cannot make the browser do it. No CORS headers are sent. |
| Machine access | The access token (`http_token`) opens only: `GET /snapshot.jpg`, `GET /status.json`, `GET /api/events`, `GET /api/event_snapshot/<n>`, `GET /api/captures`, `GET /api/capture/<n>` and `POST /api/capture` (takes a photo stored on the board; also needs the CSRF header). It cannot change settings, delete anything, reach GPIO or restart the board. Compared in constant time. Whoever has the token can see the camera, so treat it like a password (the Telegram bridge config holds it). |
| Input checking | Every setting goes through the same range checks as the config file; paths (data, web, model, auth) cannot be changed over the network; JSON bodies are limited to 16 KB and 64 keys; snapshot names must be digits + `.jpg`; static files only from the web folder, no `..`, no hidden files. |
| System actions | Only `restart_service`, `reboot`, `shutdown`, each with an explicit confirmation; no shell access. |
| Secrets | The MQTT password and the token are never returned by the API; backups and the `.gitignore` keep secrets out of the repository (checked before every push). |
| Browser | `Content-Security-Policy` (own files only), `X-Frame-Options: DENY`, `nosniff`, `no-store`. |
| Network exposure | Without a dashboard password or token the web server listens on 127.0.0.1 only. |

Checked on the real board: anonymous requests get 401; the access token opens only its listed endpoints (re-checked after adding event/photo access: config, delete and anonymous requests still get 401); a rotated token stops working; POST without the CSRF header gets 403; bad settings get 400. The same rules have unit tests (`tests/test_api.cc`, `tests/test_auth.cc`) that run in a few seconds.

## What is NOT protected (read this)
- **No encryption.** The dashboard is plain HTTP and MQTT is plain TCP: a person on your network can read passwords as you type them. Use it on a trusted home network only; do not forward ports to the internet. A TLS reverse proxy (for example Caddy or nginx on another machine) in front of port 8080 is the right fix; not provided.
- **RTSP has no password.** The Rockchip RTSP library has no authentication (the stock app is the same). Anyone on the network can watch the stream.
- **The stock firmware services are still on.** Telnet (port 23, `S50telnet`) and USB debugging (`adbd`) run with the default `root` account. **Change the root password now** (`passwd` over SSH) and consider `chmod -x /etc/init.d/S50telnet` (effective after reboot). This project did not change them, to avoid locking you out.
- Logs and events live in RAM and may contain IP addresses of failed logins.
- A device on your network can still read the board's flash through SSH if it has the root password; SSH key login is set up for the PC that installed the project.
- No firewall rules, no automatic updates, no intrusion detection.

## Passwords and tokens on your PC
`scripts/install.sh` leaves two files in your WSL home: `~/.aid_admin_password` (dashboard password) and `~/.aid_token` (access token), both mode 600. Change the dashboard password in Settings, "Change password"; delete the file afterwards if you do not want it kept.
