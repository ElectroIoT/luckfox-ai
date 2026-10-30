# Build environment (Windows 11 + WSL2) – status

Tested on the PC, 2026-10-30. Board-side steps are marked separately.

## Installed and verified on the PC
- WSL2 with Ubuntu 22.04.5 (`wsl -d Ubuntu-22.04`), Linux user `manoranjan`, ~954 GB free, 15 GB RAM visible, 16 CPU threads.
- Build packages (gcc 11.4, g++, make, cmake 3.22, bison, flex, fakeroot, dtc, unzip, pip3) installed with `apt` as root (`wsl -u root`).
- Luckfox SDK (shallow clone, ~4.6 GB on disk): `~/luckfox-pico` inside Ubuntu, commit `824b817f8` (2026-03-22). Keep it in the Linux home, not on `/mnt/d` (slow, permission problems).
- Cross-compiler `arm-rockchip830-linux-uclibcgnueabihf-gcc` 8.3.0 (crosstool-NG) compiles a test program on the PC: result is `ELF 32-bit ARM, EABI5, interpreter /lib/ld-uClibc.so.0`.

## Tested on the board (2026-10-30)
- `hello` built on the PC with the SDK cross-compiler was copied to the board with `scp` (to `/tmp`) and ran: it printed `hello luckfox`. So PC-built ARM/uClibc programs run on this Pico Pro. Camera, NPU and MPP/RKMPI code have not been run yet.

## NOT yet verified
- A full SDK firmware build (`./build.sh`) – not attempted; likely not needed.
- RKNN-Toolkit2 1.6.0 install and a model conversion.

## SDK findings
- `project/app/rkipc` holds the source of the stock camera/RTSP app running on the board.
- `media/luckfox/examples` has only peripheral tests (adc, gpio, i2c, pwm, spi, uart). No RKNN/YOLO/RTSP example inside the SDK.
- Separate repo `LuckfoxTECH/luckfox_pico_rkmpi_example` exists (57 MB, last push 2025-04-29); contents not inspected yet.

## Flashing (done on this board)
Firmware `Luckfox_Pico_Pro_Max_Flash_250607` written to SPI NAND with SocToolKit (RV1106, one-click Firmware/Upgrade, board in loader mode). See `hardware.md` for board results.
