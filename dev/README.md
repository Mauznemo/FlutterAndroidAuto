# Not part of the plugin

Everything in this directory drives the machine this plugin was written on. None of it
ships, nothing in `tools/` or in the packages calls into it, and deleting the whole
directory would leave the plugin building and running exactly as it does now.

It is here rather than outside the repository because the milestones were verified with
it, and a measurement is worth less without the thing that took it.

What it assumes, all of which is one machine rather than a requirement:

- KDE Plasma on Wayland, one 1920x1080 output at scale 1, so screenshot pixels equal
  screen coordinates
- a German (QWERTZ) keyboard layout, which is why `ui.sh` has a `paste` at all
- passwordless sudo, though every script here now says so rather than assuming it
- one paired Pixel 8 Pro, for the wireless scripts

`dev-environment.md` is the full inventory, as it was on 2026-09-12.

| File | What |
|---|---|
| `ui.sh` | screenshots and synthetic input, so the app can be looked at with nobody at the keyboard |
| `run-example.sh` | build and launch the example app, in the foreground, detached, or straight from the bundle |
| `wireless-test.sh` | drive a real wireless attempt end to end and name the stage it reached |
| `wireless-capture.sh` | record Bluetooth and Wi-Fi while that happens, because nothing above the transport can tell a phone that never asked from one that asked and walked away |
| `fake-wireless-phone.py` | stand in for a phone on the `AA_WIRELESS_FAKE_PHONE` socket |
| `audio-graph.sh` | dump the PipeWire graph, `--watch` to record it over time |
| `release.sh` | cut a release: bump, changelog, publish to pub.dev, tag, GitHub release. `--dry-run` checks everything and pushes nothing |
| `release_support.py` | the fiddly half of that: versions, pubspecs and changelogs |
| `dev-environment.md` | the machine, and what it can and cannot be driven to do |

`release.sh` is the odd one out here. It assumes nothing about this laptop and would
work on any machine, but it publishes this project's packages and pushes to this
project's repository, so it is the maintainer's tool rather than a contributor's. That
is the line `dev/` draws. See [`docs/releasing.md`](../docs/releasing.md).

The counterpart is `tools/`, which is for anyone who clones this repository:
provisioning, the aasdk build and its patch, the echo canceller, and the access point.
