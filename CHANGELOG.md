# Changelog

## 1.4.2
- **Fixed random restarts:** reading weather over plain HTTP could busy-wait while data trickled in, starving the system's idle task until the watchdog restarted the clock. The response is now read with short pauses, then parsed.

## 1.4.1
- **Weather fix:** requests now use plain HTTP first (no TLS, which needed ~40 KB of free memory), falling back to HTTPS. Weather had stopped loading on some clocks since 1.3 because memory ran short.
- The audio engine's 28 KB of working memory is now claimed only while a sound plays.
- Weather status (last update, or the last error) and memory details on the Info screen and the settings page.
- **Restart log:** the reason for each restart (power on, update, crash, freeze, power dip) and, after a crash, where it happened, on the settings page (System → Restart log).

## 1.4.0
- **Screenshot** button on the settings page: captures the clock's screen as a PNG.
- **Desk clock mode** (renamed from "home clock"):
  - stays on across restarts until you leave it
  - appears automatically when there are no upcoming events (Settings → "Desk if no events", or the checkbox on the settings page)
  - shows a countdown line for your next event
- README: light-sensor backlight bleed fix, 3D-printed enclosure section, screenshots.

## 1.3.0
- Renamed to **Countdown Clock**: "trips" are now **events**, for any kind of countdown.
- Events are kept in **date order** (soonest first) on the clock and the web page.
- **Faster weather**: all locations are fetched in one request (with a per-location fallback), refreshed every 15 minutes instead of 30.
- **Back arrow** returns to the screen you came from (Menu, Settings) instead of always to the countdown.
- Creator credit (jhollin1138) on the Info screen, startup screen and settings page.
- Builds also save `firmware/countdown-clock-<board>-v<version>.bin` for wireless updates; the settings page shows the installed version.
- Home clock: wider 4-day forecast with chance of rain.
- Speaker: the DAC is only powered while a sound plays, which removes idle static.
- Audio engine rewritten without GPL code: own RTTTL and WAV players, MP3 decoding with public-domain minimp3 (skips ID3 tags and album art). Ringtone pitch is now exact.
- Licensed under the PolyForm Noncommercial License 1.0.0.

## 1.2.0
- **Sound**: volume control (web page and on-screen menu), MP3/WAV clips and RTTTL ringtones, separate milestone and celebration sounds, three built-in tunes. Audio now uses the ESP32's DAC, which removes the harsh square-wave buzz.
- **Shading presets per trip**: None / Light / Medium / Dark behind the text. Light (the new default) is about half as dark as before.
- **Wireless firmware updates** from the settings page (new partition layout with two app slots).
- **Backup & restore** of trips and settings.
- Removed the photo slideshow. Each trip has one background.
- Touch: tapping a celebration screen also stops its sound.

## 1.1.0
- Touch screens: menu, weather detail with 5-day forecast, home clock with home weather, trip editor with on-screen keyboard and date picker, WiFi setup, settings, touch calibration.

## 1.0.0
- First release: countdown, destination weather, photo backgrounds, auto-brightness, quiet hours, milestones.
