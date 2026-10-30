# microSD card: lasting history and photos

**Status: written and unit-tested on the PC (card detection logic, storage code), NOT tested with a real card.** No microSD card was available during development, and the SDK I examined does not show how the board mounts one. Treat the first run as a test and read "What to check" below.

## What it does
With `data_dir=auto` (the default) the program looks for a memory card when it starts:

| Situation | Where events and pictures go |
|---|---|
| A card is already mounted (any `/dev/mmcblk*` device in `/proc/mounts`) | `<mount point>/aid-data` |
| A card is inserted but not mounted | The program tries to mount it read-write at `/mnt/sdcard` (FAT32 `vfat`, then `exfat`, then `ext4`) and uses it if that works |
| No card, or it cannot be written | RAM (`/tmp/aid-data`), lost at reboot, exactly as before |

On a card the history is longer: at least 2000 events and 1000 event pictures are kept (more if you set larger numbers). Event pictures are `aid-data/snapshots/<event id>.jpg`, the event list is `aid-data/events.jsonl`, and photos taken with the dashboard button or the Telegram `/photo` command go to `aid-data/captures/<time in ms>.jpg`. Event numbering continues after a reboot, so older pictures are never overwritten.

Nothing is written to the board's SPI flash, with or without a card. A card also takes load off the RAM disk.

## How to use it
1. Format the card as FAT32 (or exFAT / ext4) on your PC. A 8 to 64 GB class-10 card is plenty.
2. Power the board off, insert the card into the slot on the underside of the board, power on.
3. Dashboard, **Dashboard** page: the storage box shows "Memory card (events and photos)" with free space. If it says "RAM only", the card was not found or could not be used; look at the System page's log for `memory card` lines.
4. Take a photo (Live camera page, "Take photo (keep on board)") and check that it appears under "Stored photos".

**Hot-plugging is not handled:** insert the card while the power is off, or restart the service (System page, "Restart service") after inserting it.

## Reading the files
- Dashboard: Event history shows the pictures; Live camera lists stored photos.
- Remove the card and read it on a PC (FAT32/exFAT works everywhere; ext4 needs Linux).
- Over the network: `scp root@<board>:/mnt/sdcard/aid-data/captures/*.jpg .`
- Sharing "automatically" through a network folder (SMB/Samba) is **not** built in; the board has no Samba server. Use the Telegram bridge or scp/rsync.

## What to check on first use
- `mount | grep mmcblk` over SSH shows where the card is mounted.
- `dmesg | tail -30` shows card detection or errors. Look for "mmc" lines.
- The dashboard storage box and `/status.json` (`system.sd`) show `present`, `mounted`, `in_use`, free and total MB.
- If the card is present but not mounted by the program, the log line says why (for example an unsupported file system). Mount it by hand (`mount /dev/mmcblk1p1 /mnt/sdcard`) and restart the service to see whether the program then picks it up.
- Remove the card only with the board off. Pulling it while the program writes can corrupt the FAT file system.

## Limits
- One card, one mount point. Spaces or special characters in the mount point are not supported.
- The program never formats or repartitions a card.
- Video recording to the card is **not implemented** (see `docs/ROADMAP.md`); only pictures and the event list are stored.
- Card wear: event pictures are written one at a time when something happens (not continuously), which is gentle. Constant video recording would need a "high endurance" card.
