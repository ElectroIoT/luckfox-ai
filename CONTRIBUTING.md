# Contributing

Thanks for helping. This project is a hobby-scale appliance for one specific board (Luckfox Pico Pro / RV1106G2, 128 MB RAM), so a few rules keep it trustworthy.

## Ground rules
1. **Say what was tested and where.** Every claim in the docs is one of: tested on the board, tested on the PC only, not tested. Please keep it that way. Never write a performance number you did not measure, and say how you measured it.
2. **Protect the board.** Do not add code that writes to the SPI flash while running, forks helper processes in a loop, or grows memory. RAM is about 28 MB free with everything running.
3. **No secrets in the repository:** tokens, passwords, bot tokens, Wi-Fi keys, MAC addresses, enrolled face photos, camera pictures of your home. `.gitignore` covers the usual places; check `git diff --cached` anyway.
4. **Small, boring code.** No new third-party libraries on the board (there is no package manager); PC tools use the Python standard library unless the feature is optional.

## Set up
```bash
bash scripts/test.sh        # PC tests (C++ with g++, Python with python3), no board needed
bash scripts/build.sh       # cross-build for the board (needs the Luckfox SDK, see docs/installation.md)
bash scripts/web_demo.sh    # dashboard with fake data on http://127.0.0.1:18090 (login in the script header)
```
Add a test for every new rule or route (`tests/`). The test programs are plain `CHECK(...)` files; the Telegram bridge uses `unittest`.

## Pull requests
- One topic per pull request, with the test output in the description and the label (board / PC / untested) for what you verified.
- Keep the code style of the file you edit: comment density, naming and idiom.
- Documentation changes are welcome, especially corrections from real use.

## Reporting problems
Open an issue with: board model and firmware image, the `aid-core` version (`/root/aid/aid-core --version`), the System page log or `tail -100 /tmp/aid-core.log`, and `dmesg | grep -i -E "sfc|ubi|mmc"` if storage is involved. Remove tokens and passwords first.

## Security issues
Please do not post exploits publicly. Open a private security advisory on the repository (or contact the maintainer) with details.

## License
By contributing you agree that your contribution is licensed under the MIT License of this project (`LICENSE`). Code under `src/ai/third_party/rockchip/` keeps its Apache-2.0 license.
