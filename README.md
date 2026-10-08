# PS5 NFC Launcher

Launch your installed PS5 and PS4 games by placing an NFC card on a reader connected to your PS5.

Manage your games and cards through a web interface on your phone or computer. Once set up, card launching runs directly on the PS5—you do not need to keep the browser or a computer running.

Inspired by the amazing [Zaparoo](https://zaparoo.org/) project. This is an independent implementation and is not affiliated with Zaparoo.

## Features

- **Tap mode:** Scan a card to launch its game. Removing the card leaves the game running.
- **Hold mode:** Keep the card on the reader to play. Removing it closes the game launched by that scan.
- Browse installed games with icons, search, and PS4/PS5 filters.
- Launch and close games from the web interface.
- Assign cards to games without changing their contents.
- Write game information directly to compatible NFC cards.
- Manage saved card assignments and adjust the card removal delay.

## What you need

- A PS5 with a homebrew environment that can load ELF payloads.
- An **ACS ACR122U** USB NFC reader.
- Compatible NFC cards. For writing, use unlocked **NTAG213, NTAG215, or NTAG216** cards.
- A phone or computer on the same network as your PS5 for setup.

The project has been tested on **PS5 firmware 13.60 with Relapse**. Other firmware versions have not been verified.

**Reader support:** The ACR122U is working in testing. PN532 USB reader support is still being investigated and is not currently working reliably.

## Getting started

Only source code is currently provided. You will need to build the payload before testing.

1. Clone or download this repository and build the ELF payload.
2. Start Relapse and your usual payload loader.
3. Connect the ACR122U to a USB port on your PS5.
4. Load the compiled ELF using Payload Manager or PS5Upload's payload sender.
5. On your phone or computer, open:

   ```text
   http://YOUR-PS5-IP:8087
   ```

   For example: `http://192.168.1.100:8087`.

6. Check that the reader is ready and your installed games appear.

The web interface is included in the payload. There are no separate website files to install.

Run only one instance of the payload. Reload it after restarting your PS5 or homebrew environment; automatic startup is not included.

## Assign a card without writing to it

1. Place a card on the reader.
2. Select its game in the web interface.
3. Use the detected card ID and save the assignment.
4. Remove the card and scan it again to launch the game.

Assignments are saved on your PS5. You can view and remove them in the web interface.

A saved assignment takes priority over game information written on the card. Removing an assignment does **not** erase the card: if it still contains a game value, that value can launch the game again.

## Write a game to a card

1. Select a game and choose **Write to NFC card**.
2. Confirm that you want to replace the card's existing NFC contents.
3. Place a compatible card on the reader within two minutes, or keep the card already on the reader in place.
4. Leave it there until writing and verification succeed.
5. Remove the card and scan it again to launch.

To rewrite a card that already launches a game, choose **Write to NFC card** before placing it on the reader. If the card is already present, leave it in place while writing.

New writes use a standard **NDEF text record** containing a value such as `ps5nfc:PPSA12345`. The card stores a game identifier, not the game itself. Older cards written as URI records remain supported.

A written card can be used with this launcher on another PS5 if the corresponding game is available there. The value is not currently a supported Zaparoo launch command.

Writing replaces the existing NFC contents. Locked or password-protected cards are rejected. If writing fails, keep the card in place and retry; its previous contents may have changed.

## Tap and Hold modes

Choose your mode in the web interface and save it. Changes apply the next time you place a card on the reader.

In Hold mode, automatic closing applies to the game started by that card scan. A game that was already running is not automatically claimed for closing. Disconnecting the reader or losing communication leaves the game running.

## Troubleshooting

- **The page does not open:** Check the PS5's IP address, confirm both devices are on the same network, and make sure the payload is running.
- **The reader is not ready:** Use an ACR122U and connect only one NFC reader. PN532 support is still a work in progress.
- **A card does not launch:** Check its assignment or written game value, then remove and rescan it. The game must already be installed or registered and available on the PS5.
- **A removed assignment still launches:** The card may contain a written game value. Removing the assignment does not remove that value.
- **Writing fails:** Use an unlocked NTAG213, NTAG215, or NTAG216 and keep it on the reader until verification finishes.
- **Updating the payload:** Use **Stop payload** in the web interface before loading the new ELF. Refresh the page afterward.

For reader problems, use **Download diagnostic log** in the web interface and include the log when reporting an issue.

## Project status

This is an early project under active development. ACR122U launching, card management, and writing have worked in testing, but wider hardware and firmware compatibility is still being checked.

The launcher does not install games or mount game images. Games must already be available on your PS5.

Created by **Anime0t4ku**.

## Building from source

Install Python 3, make, and the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) with its required toolchain. The project was built using SDK v0.43 and LLVM 18.1.3. Set the SDK location, then run:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make
```

The result is `ps5-nfc-launcher.elf`. The web interface is embedded automatically during the build.

## Credits

- [PS5Upload](https://github.com/phantomptr/ps5upload) by phantomptr and contributors: the bundled installed-game database reader, and source references for game launching and closing.
- [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble and contributors: the bundled JSON library.
- [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) by John Tornblom and contributors: the SDK used to build the payload.
- [PuckbridgePS5](https://github.com/ThisIsAkill/PuckbridgePS5): a reference for PS5 USB access and game metadata handling.
