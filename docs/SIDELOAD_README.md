# Sideloading the Predator-RF APK to a Samsung Galaxy S22

You built the APK on Windows from `github.com/JakeTheSnake0245/Predator-RF`
(Path 2). This is how to get it onto the S22 without going through
the Play Store. Should take ~5 minutes.

## 0. Once-only setup on the phone

1. Settings → About phone → tap **Build number** 7 times → "Developer mode is ON"
2. Settings → Developer options → enable **USB debugging**
3. Settings → Developer options → enable **Install via USB**
4. Plug into the Windows box; tap **Allow** on the RSA fingerprint prompt

## 1. Once-only setup on the Windows box

Install Android Platform Tools (just `adb`, you don't need the SDK):

```
winget install Google.PlatformTools
```

Or download `platform-tools-latest-windows.zip` from
https://developer.android.com/tools/releases/platform-tools and unzip
to `C:\platform-tools`. Add it to your `Path`.

Verify:

```
adb devices
```

You should see `R3CXXXXXXXXX  device`. If you see `unauthorized`, the
fingerprint prompt didn't accept — re-plug, accept on the phone.

## 2. Build the APK on Windows

In the Predator-RF repo root, **start with the debug build** — it's
auto-signed with the standard Android debug keystore, no extra setup:

```
.\gradlew clean assembleDebug
```

The signed-by-debug-key APK lands at:

```
app\build\outputs\apk\debug\app-debug.apk
```

The reference `android/sample/build.gradle.kts` in this repo
intentionally leaves `release` UNSIGNED — running `assembleRelease`
without first wiring a real `signingConfigs.release` produces an
unsigned APK that `adb install` rejects (`INSTALL_PARSE_FAILED_NO_CERTIFICATES`).
For personal sideloads to your own S22, debug-signed is fine; for
distribution, generate a keystore (`keytool -genkeypair`) and wire it
into `signingConfigs.release`.

## 3. Push to the phone

```
adb install -r .\app\build\outputs\apk\debug\app-debug.apk
```

`-r` means "replace if already installed" — preserves app data.

If you get `INSTALL_FAILED_VERSION_DOWNGRADE`, you're trying to install
an older versionCode than what's on the phone. Either bump
`versionCode` in `build.gradle` or `adb uninstall com.jakethesnake.predatorrf`
first (this WIPES app data).

## 4. Configure on the phone

Open Predator-RF on the S22:

1. Settings → **Backend URL** → `http://<RPi-IP>:8000`
   (find the IP with `hostname -I` on the RPi)
2. Settings → **Bearer token** → paste from
   `/etc/predator-rf/predator-rf.env` (`API_BEARER_TOKEN`)
3. Tap **Test connection** — should green-tick within 2 s.

## 5. Verify the link

On the RPi:

```
journalctl -u predator-rf -f | grep android-pull
```

You should see the phone's IP polling every ~5 s.

On the phone:

* Banner at the top must show **GO** (mirror of `preflight_go`)
* Tracks tab shows live tracks within 10 s of the backend seeing them
* Approvals tab shows any pending CoT approvals; tap to approve/reject

## 6. Common sideload gotchas

* **"Install blocked":** Settings → Apps → Special access → Install unknown apps → enable for whatever pushed the APK (USB Drive, your file manager, etc.)
* **App opens but can't reach backend:** the S22 is on cellular, not on the LAN with the RPi. Either join the same Wi-Fi or stand up a tailscale / wireguard tunnel. Don't expose the backend to the public internet without TLS.
* **Phone says "GPS lock unknown":** that's the BACKEND'S preflight, not the phone's GPS. Check `journalctl -u predator-rf -f` for fleet-node GPS warnings.
* **CoT markers don't appear in ATAK:** open `docs/ATAK_COT_FORMAT.md`. The backend default UDP destination is `239.2.3.1:6969` (TAK SA multicast). On a phone hotspot, multicast is dropped — use the HTTP-pull mode (`GET /api/v1/cot/export`) and ATAK's local file-import instead.
* **Battery drain:** the polling cadence is 5 s on Wi-Fi by design.
  In Settings, set **Cellular cadence** to 30 s if you'll be on
  carrier data for hours.
