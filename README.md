Start with REPAIR_NOTES.md for this repair and the required reinstall steps.

# MellorsOS 2.0 — storage, notifications, files and store apps

This update is based on your actual MellorsOS 2.0.1 compile-fix source. The displayed OS version is **2.0**, as requested. Use the firmware and IDE from this package together. The three new apps require the updated runtime's memory and LAN discovery APIs.

## Install the firmware

1. Extract this ZIP.
2. Open MellorsOS_CyberPi_v2_0/MellorsOS_CyberPi_v2_0.ino in Arduino IDE. Keep its companion .cpp and .h files in that folder.
3. Keep the board, flash size and partition scheme that worked on your CyberPi. The build here was verified with ESP32 board package **2.0.17**, **ESP32 Dev Module**, **8 MB flash**, **Huge APP**, **PSRAM enabled**, **DIO**, **80 MHz flash**, **240 MHz CPU**, and **115200 upload speed**.
4. Verify, then upload using your CyberPi's current COM port. This package does not upload automatically.
5. Existing installed apps under /installed migrate into the private package area on boot. This update preserves files, Wi-Fi, permissions and app data instead of resetting them for every new build date.

Keep a backup and retain your working partition scheme: changing a partition layout can make an existing filesystem inaccessible. The update refuses to auto-format an unmountable existing filesystem. A first-ever installation can initialize a blank filesystem.

## App storage

- Installed app source now lives at /system/apps/PACKAGE_ID/main.mapp.
- The on-device Files app, its file picker and MScript file APIs exclude protected app-package paths. Native file operations cannot copy/move/rename into those paths.
- Downloaded .mapp files in public folders are independent installer copies. Deleting a copy no longer deletes an installed app.
- The public app-data folder remains /apps/PACKAGE_ID. User-created drawings and other app files live there.
- Installation stages a complete file before replacement. A failed write preserves the previous app. Uninstall through App Manager.
- This is a protected namespace on the existing LittleFS storage, not a new physical flash partition. No partition-table change is required.
- App capacity is now 24 total apps, including built-ins.

## Notifications in Quick Settings

On Home, move to the top row and press Up to open Quick Settings. Select **Notifications**.

Open a notice to read its title, body and sending app. Choose **Delete notification** or keep it. **Clear all** removes the history after confirmation. Notices are shown newest first. Deleting a notice uses its stable ID so newly arriving messages do not change which notice is deleted.

History remains in RAM, as in the original firmware; it does not survive a restart.

## MConnect Files

Keep the MConnect app open on CyberPi, then visit its IP address in your computer's browser. Choose **Files** and enter the device lock PIN.

You can browse any filesystem path, create folders, upload/download arbitrary binary files, create/edit UTF-8 text files, rename/move files, and delete files or empty folders. Listings load 64 entries at a time. Uploads stage complete binary content before replacing the destination.

As requested, authenticated MConnect has access to **all LittleFS files**, including /system/apps. Editing or deleting those internal packages can change or break installed apps; restart after deliberately changing package contents directly. Filesystem access does not expose the separate NVS settings database as files.

The on-device Files app stays restricted even though the authenticated MConnect administrator can inspect internal files.

## Updated IDE

Open MellorsOS_MScript_Studio_v2_0.html.

The **Apply** button is at the bottom of the sidebar. It is the explicit action that copies the sidebar name, package, version, color and icon into the code. Save locally, Export and Install use the editor's code as written. Export filenames use the package ID in the source, so stale sidebar values do not rename a pasted app.

## Install the new apps

1. Upload this firmware first.
2. Open MConnect on CyberPi. Enable Developer Mode for IDE sideloading and set a lock PIN.
3. Open the included IDE, select **Import .mapp**, and choose a file from Store_Apps.
4. Enter the device IP and choose **Install .mapp**. Enter the lock PIN.
5. Launch the app from Home. Repeat for each app.

For store installation, upload the three .mapp files and add their catalog entries as described in Store_Apps/PUBLISH.md. They are packaged for publication; this chat has not published them to the live GitHub store.

## Mellors Draw

48 × 40 editable pixels, displayed at 2× scale. Includes pen, eraser, line, rectangle, ellipse, flood fill, eyedropper, 16 palette entries (including one custom RGB color), brush widths 1–4, eight undo/redo levels, save/open and PPM image export.

- Stick moves the cursor. Hold A for pen/eraser. Tap A at each endpoint for shapes. Tap A for fill or eyedropper.
- B opens the tool/file menu. Choose Resume canvas to return.
- Select a color or enter #RRGGBB for the custom palette entry. Changing that palette entry recolors existing custom-color pixels.
- Saves use the MDRAW1 text format. Use an .mdraw filename. Export writes a standard P3 .ppm image beside the drawing. Download either through MConnect Files at /apps/tools.mellorsdraw.
- Drawings and undo history remain in RAM until saved. Exiting without saving discards them. There are no layers or general photo import; this is a pixel-art editor for the CyberPi display.

## Minesweeper

| Difficulty | Board | Mines |
| --- | --- | --- |
| Easy | 6 × 6 | 6 |
| Normal | 8 × 8 | 10 |
| Hard | 10 × 10 | 18 |

Stick moves; A reveals a square. **Up+A** toggles flag mode, then A adds/removes flags. Open a numbered square again to chord when its surrounding flag count matches. The first revealed square is always safe. Empty regions expand automatically. Reveal all safe squares to win; flags alone do not win. Incorrect flags can cause a chording loss.

B returns to difficulty selection for a new game. Best completion times are stored separately for each difficulty and shown in the menu. The timer pauses when the app is suspended.

## LAN Tic Tac Toe

Install the new firmware and this app on **both CyberPi devices**.

1. Connect both to the same LAN and grant the app Network permission.
2. On player one's device choose **Host a game (X)**. Keep the app open.
3. On player two's device choose **Find LAN games (O)**, select the host and join.
4. The scan queries the same _mellors._tcp mDNS service as native Messages, then checks each discovered peer for an available Tic Tac Toe server on port 8082.
5. If multicast discovery is unavailable, enter the host IP shown on its display.

Stick selects a square; A places your mark. X starts. The host validates player sessions, turn order, occupied squares and round numbers. A third player is rejected. The client polls the host for current state, so a lost response can be recovered by the next poll. After a result, the host presses A to start the next round.

B leaves the game. Suspending or closing the host stops its server; reconnect from the menu. A disconnected guest times out after about 20 seconds. Discovery needs a LAN that permits multicast and communication between clients; guest Wi-Fi networks may isolate devices.

## Verification

See VALIDATION.txt for firmware build results, interpreter tests, storage/notification tests, IDE regression checks, and their limitations. No firmware was uploaded and no physical CyberPi or real two-device LAN playtest was performed here.
