# Firmware, flashing, recovery

## What this project runs on
- Board: **Luckfox Pico Pro** (RV1106G2, 128 MB RAM, 256 MB SPI NAND). The firmware reports the model string "Luckfox Pico Pro Max" for both the Pro and the Max; the 128 MB RAM range showed it is the Pro.
- Firmware flashed during development: the official Luckfox Buildroot image **Luckfox_Pico_Pro_Max_Flash_250607** (Buildroot 2023.02.6, kernel 5.10.160, uClibc), written to the SPI NAND with SocToolKit. The board shipped with only a factory test image.
- Nothing in this project flashes or changes the firmware. `scripts/install.sh` copies files into `/root/aid` and adds one boot script, `/etc/init.d/S99aid`. Everything is undone by `scripts/uninstall.sh`.

## Flashing the official image (Windows 11, done once during setup)
1. Install the Rockchip USB driver (DriverAssistant from the Luckfox wiki Downloads page) and restart the PC.
2. Download the Buildroot "Flash" image for Pico Pro/Max and SocToolKit from the wiki; extract the image into its own folder.
3. Start SocToolKit as administrator, choose RV1106, storage **Flash** (the Pro has no eMMC).
4. Put the board in flash mode: hold the **BOOT** button while plugging in USB, or run `reboot loader` on a running board.
5. Choose the image folder, click **Upgrade** (one-click) or select partitions and **Download**.
Wiki page with the same steps: https://wiki.luckfox.com/Luckfox-Pico-RV1106/Getting-Started/Pico-Pro-Max
Use a good USB-C data cable. Power the board from a **separate 5 V / 2 A adapter** while it runs the camera (see "Power" below).

## Backups
- `bash scripts/backup.sh` saves the settings (`aid.conf`, `auth.json`), the event file and the boot script to `backups/<time>/` (git-ignored, contains secrets).
- `bash scripts/backup.sh --firmware` also reads the small raw partitions (env, idblock, uboot, boot, oem, userdata, about 46 MB) from the flash. **Not run during development**: it puts read load on the flash, which showed timeouts in testing. The 210 MB rootfs is not included; flash the official image to get it back.
- Restore settings with `bash scripts/restore.sh backups/<time>`. Raw partition images are not written back by any script here; use the Rockchip tools.

## Recovery
| Situation | What to do |
|---|---|
| The camera program keeps failing | The supervisor restarts it; after 5 quick failures in a row it starts the stock camera app instead. |
| You want the stock app back for good | `ssh root@<ip> "touch /root/aid/disabled"` and reboot. Or `bash scripts/uninstall.sh`. |
| Dashboard unreachable, SSH works | `ssh root@<ip> "/etc/init.d/S99aid status"`, look at `/tmp/aid-core.log`, `/etc/init.d/S99aid restart`. |
| Flash errors: files fail with "Input/output error", `/` is read-only | The supervisor reboots the board by itself (after 5 minutes of uptime). By hand: `/bin/busybox reboot -f`. |
| Board does not boot at all | Flash the official image again (needs a PC, the BOOT button and the steps above). Your settings are in the backup. |
| Lost dashboard password | `ssh root@<ip> "/root/aid/aid-core --set-password"` sets a new one (reads it from the terminal). |
| A bad update | `bash scripts/restore.sh` for settings; rebuild an older commit with `git checkout <commit>` and run `scripts/build.sh` and `scripts/deploy.sh`. |

## Power
Development showed SPI flash controller timeouts (`rockchip-sfc ... DMA wait for transfer finish timeout`) four times, first while the board was powered from a PC USB port. After switching to a 5 V adapter the board ran for long stretches without errors, but the cause was **not proven** to be power. Use an adapter rated 5 V / 2 A or more (a 5 V / 3 A adapter is in use now; the board then ran almost 2 hours without a flash error, which fits but does not prove the power theory) and a short, thick cable. See `docs/streaming.md` for the full account and the safeguards (no flash writes while running, supervisor reboot on a read-only root).

## Things that were not done
- No custom kernel or device-tree changes (so the hardware watchdog, which this kernel does not expose, is not available; the supervisor is a software substitute).
- No microSD test (no card was available), no eMMC (the Pro has none).
- Over-the-air firmware update is not implemented.
