# Changelog

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
