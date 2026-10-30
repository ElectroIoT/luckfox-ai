# Home Assistant integration (MQTT)

## Quick start: connect the camera to Home Assistant, step by step
You need Home Assistant running (on a Raspberry Pi, a mini PC, a VM or Home Assistant Green) on the same network as the board.

1. **Install the MQTT broker.** In Home Assistant: Settings, Add-ons, Add-on store, search **Mosquitto broker**, Install, Start, tick "Start on boot". (Already have a broker somewhere else? Use its address in step 4.)
2. **Create an MQTT user.** Settings, People, Users, Add user, for example `camera` with a password (a normal user is enough). Write the password down.
3. **Add the MQTT integration.** Settings, Devices & services, a "Discovered MQTT" card may appear: click Configure, Submit. If not, Add integration, MQTT, broker `core-mosquitto` (or the Home Assistant IP), port 1883, the user and password from step 2.
4. **Tell the camera about the broker.** Open the camera dashboard (`http://<board-ip>:8080/`), Settings, "MQTT / Home Assistant": tick Enable, Host = your Home Assistant IP, Port 1883, the user `camera` and its password, leave the other fields as they are, Save, then System page, **Restart service**.
5. **Check.** Settings, Devices & services, MQTT, Devices: **Luckfox AI Camera** appears with the entities from section 2. Walk in front of the camera: *Person detected* turns on. If nothing appears: the camera's Settings page shows the last MQTT error (wrong password, unreachable broker). `mosquitto_sub` (section 6) shows the raw messages.
6. **Add the video.** Settings, Devices & services, Add integration, **Generic Camera**: stream source `rtsp://<board-ip>:554/live/0`, still image URL `http://<board-ip>:8080/snapshot.jpg?token=<http_token>`, RTSP transport TCP, leave authentication empty, Submit. The camera now appears as an entity you can put on a dashboard card (Picture glance or Picture entity).
7. **Make it useful.** Add the automation from section 5 (phone notification when a person is detected), or in the automation editor choose the trigger "Person detected" of the Luckfox AI Camera device. With a notification that includes a picture:
```yaml
action:
  - service: notify.mobile_app_your_phone
    data:
      message: "Person at the camera"
      data:
        image: "/api/camera_proxy/camera.luckfox_ai_camera"   # the entity name of step 6
```
8. **Do not expose anything to the internet.** MQTT and the camera use no encryption. For remote access use Home Assistant's own secure access (Nabu Casa or a VPN), not port forwarding to the board.

Telegram alerts with photos, a menu and video are in `docs/telegram.md`; they can run next to Home Assistant.

The sections below are the reference.

**Status:** the MQTT client and the discovery messages are tested on the PC (packet bytes against the MQTT 3.1.1 spec, a fake broker with login, retained messages, refusal and automatic reconnect, and the discovery JSON). They were **not tested against a real broker or a real Home Assistant**; the first connection on your network may need small fixes. MQTT here is plain TCP, no TLS: use it on your home network only.

## 1. Turn it on
Dashboard, Settings, "MQTT / Home Assistant": tick Enable, enter your broker address, port, user and password, then Save and restart the service. Or edit `/root/aid/aid.conf` on the board:
```
mqtt_enabled=1
mqtt_host=192.168.1.5
mqtt_port=1883
mqtt_user=ha
mqtt_password=your-password
mqtt_prefix=luckfox-ai
mqtt_device_name=Luckfox AI Camera
mqtt_discovery_prefix=homeassistant
```
The password is stored only in that file on the board and is never shown by the dashboard or the API.

## 2. What appears in Home Assistant (MQTT discovery)
If MQTT discovery is on in Home Assistant (default prefix `homeassistant`), one device "Luckfox AI Camera" with these entities shows up by itself:

| Entity | Type | State topic |
|---|---|---|
| Person detected | binary sensor (occupancy) | `luckfox-ai/detection/person` (ON/OFF) |
| Vehicle detected | binary sensor | `luckfox-ai/detection/vehicle` |
| Animal detected | binary sensor | `luckfox-ai/detection/animal` |
| Motion detected | binary sensor (motion) | `luckfox-ai/detection/motion` (ON for 30 s after movement) |
| Camera status | sensor | `luckfox-ai/camera/status` (online / disconnected) |
| AI status | sensor | `luckfox-ai/ai/status` (running / disabled / model_error) |
| Detection count | sensor | `luckfox-ai/ai/count` |
| SoC temperature | sensor (C) | `luckfox-ai/system/temperature` (every 30 s) |
| Last event | sensor + attributes | `luckfox-ai/events` (JSON) |
| Snapshot URL | diagnostic sensor | `luckfox-ai/camera/snapshot_url` |

Availability: `luckfox-ai/status` is `online` while the program runs. The broker publishes `offline` (last will) if the connection is lost, and the program publishes `offline` itself at a clean stop. All entities follow it. With another `mqtt_prefix` all topics start with that prefix.
Every event is also published (not retained) on `luckfox-ai/events`, for example:
`{"id":12,"ts":1793386219538,"type":"person_detected","label":"person","conf":0.870,"count":1,"snapshot":"12.jpg","detail":""}`

## 3. The video in Home Assistant
RTSP is not carried over MQTT. Add the stream as a camera, for example with the "Generic Camera" integration:
- Stream source: `rtsp://<board-ip>:554/live/0`
- Still image URL: `http://<board-ip>:8080/snapshot.jpg?token=<your access token>`
  (the token is `http_token` in `/root/aid/aid.conf`; with your install it is also in `~/.aid_token` inside WSL on the PC that ran `scripts/install.sh`).
The RTSP stream has **no password** (the Rockchip RTSP library has no authentication, same as the stock app). Keep the board on a trusted network.

## 4. Manual configuration (if you do not use discovery)
```yaml
mqtt:
  binary_sensor:
    - name: "Camera person"
      state_topic: "luckfox-ai/detection/person"
      payload_on: "ON"
      payload_off: "OFF"
      availability_topic: "luckfox-ai/status"
      device_class: occupancy
    - name: "Camera vehicle"
      state_topic: "luckfox-ai/detection/vehicle"
      payload_on: "ON"
      payload_off: "OFF"
      availability_topic: "luckfox-ai/status"
  sensor:
    - name: "Camera SoC temperature"
      state_topic: "luckfox-ai/system/temperature"
      unit_of_measurement: "°C"
      device_class: temperature
      availability_topic: "luckfox-ai/status"
    - name: "Camera last event"
      state_topic: "luckfox-ai/events"
      value_template: "{{ value_json.type }}"
      json_attributes_topic: "luckfox-ai/events"
```

## 5. Example automation
```yaml
automation:
  - alias: "Person at the camera"
    trigger:
      - platform: state
        entity_id: binary_sensor.luckfox_ai_camera_person_detected
        to: "on"
    action:
      - service: notify.mobile_app_your_phone
        data:
          message: "Person detected by the camera"
```
(The entity name depends on your device name; check Settings, Devices.)

## 6. Checking it from the PC
With Mosquitto's clients installed: `mosquitto_sub -h <broker> -u <user> -P <password> -t 'luckfox-ai/#' -v` shows every message. If nothing arrives, the dashboard's Settings page shows the last MQTT error (wrong password, unreachable broker).
