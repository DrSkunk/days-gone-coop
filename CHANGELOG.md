# Changelog

## [0.4.0](https://github.com/DrSkunk/theys-gone/compare/v0.3.0...v0.4.0) (2026-10-21)


### ⚠ BREAKING CHANGES

* GuestSaveGuard and ShareSave are removed; use HostSave.

### Features

* host-first save and story sync, native ride prompt, research tools ([16773d9](https://github.com/DrSkunk/theys-gone/commit/16773d9ce79e254a0b7ee272aaf03524e4d882b3))
* share host enemies with the guest, both ways ([f999df7](https://github.com/DrSkunk/theys-gone/commit/f999df74a2b0d48b3ae5e1024ca9e0b63c30a086))

## [0.3.0](https://github.com/DrSkunk/theys-gone/compare/v0.2.0...v0.3.0) (2026-10-21)


### ⚠ BREAKING CHANGES

* the plugin is now TheysGone.asi with TheysGone.ini and TheysGone.log; the old DaysGoneTogether.ini is not read. The LAN beacon magic is now "TGGB" and the Steam lobby key is "tgg", so older builds cannot find or join Theys Gone sessions.
* cut pose bandwidth with pose format v2 (protocol v6)

### Features

* cut pose bandwidth with pose format v2 (protocol v6) ([43519d1](https://github.com/DrSkunk/theys-gone/commit/43519d1bbfd8179bdc9a9277de173d4aa9231fcc))
* experimental host-to-guest save transfer ([2ca4b59](https://github.com/DrSkunk/theys-gone/commit/2ca4b59f0e61ae423e1dc78c2491f817dba37a03))
* experimental Steam lobby and networking transport ([0fb4904](https://github.com/DrSkunk/theys-gone/commit/0fb4904cebf2653fd841c068c223584a8d55da32))
* mission/save discovery trace and experimental guest save sandbox ([5ee270d](https://github.com/DrSkunk/theys-gone/commit/5ee270d6e3f7b33b4ae06820fee1648c18ee4d52))
* native CO-OP title/pause menus with Steam and LAN sessions ([febbf9a](https://github.com/DrSkunk/theys-gone/commit/febbf9a772014a4711295ca39ab2aa30c2b30521))
* rename the mod to Theys Gone ([d4eb330](https://github.com/DrSkunk/theys-gone/commit/d4eb33042cef5e6c12995f2c1559e0ffd78edfe0))


### Bug Fixes

* add .DS_Store to .gitignore ([b7dd821](https://github.com/DrSkunk/theys-gone/commit/b7dd821e8ed2174f14136e35032c438b0c94e649))
* clean CO-OP session events and HUD log placement ([84325b1](https://github.com/DrSkunk/theys-gone/commit/84325b1ab8a9ee240c0f4d159a604bc17e8e5ebe))


### Documentation

* sync Nexus copy and tested transport status ([3a96fef](https://github.com/DrSkunk/theys-gone/commit/3a96fef47781272835a8b01a019c39f41d12e601))

## [0.2.0](https://github.com/DrSkunk/days-gone-together/compare/v0.1.0...v0.2.0) (2026-10-19)


### ⚠ BREAKING CHANGES

* the files are now DaysGoneTogether.asi and DaysGoneTogether.ini (log: DaysGoneTogether.log). Delete the old DaysGoneCoop.* files from BendGame/Binaries/Win64 and copy your settings into the new INI.

### Features

* rename the mod to Days Gone Together ([b504bcb](https://github.com/DrSkunk/days-gone-together/commit/b504bcbfb7e9b70f9565ea01b5efbcb749005514))

## 0.1.0 (2026-10-19)


### Features

* embed the release version in the plugin and package release archives ([03f9a70](https://github.com/DrSkunk/days-gone-together/commit/03f9a700dd0e4a7ca9c9983efaf418de05993567))


### Documentation

* rewrite documentation as a public guide and add MIT license ([1a9135d](https://github.com/DrSkunk/days-gone-together/commit/1a9135d0080b0c4002ca45a337abbe095d9bd073))
