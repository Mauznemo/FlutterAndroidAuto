## 0.1.6

No changes. Released alongside the other android_auto packages.

## 0.1.5

### Added

- tell when the phone's picture is live

### Fixed

- connected state while recovering a lost phone

## 0.1.4

No changes. Released alongside the other android_auto packages.

## 0.1.3

No changes. Released alongside the other android_auto packages.

## 0.1.2

### Added

- fit the projection to a view of any shape
- pick the frame size from the view

### Fixed

- projection looks grainy when drawn smaller
- projection blurry after resizing the view

## 0.1.1

### Fixed

- plugin does not compile on clang 18
- lowercase username in the repository url

## 0.1.0

### Added

- build aasdk against boost 1.90
- render native video into a flutter texture
- connect to a phone over usb aoap
- show the phone's projected screen in the flutter texture
- send touch, keys and rotary to the phone
- play the phone's audio through the speakers
- send the microphone to the assistant
- add echo cancellation for calls
- report the car's sensors to the phone
- read the phone's turns, tracks and calls
- project without a cable
- cache the native build with ccache

### Fixed

- session cannot be resumed after stopping
- session does not resume after the cable is replugged
- every usb transfer failure logs as cancelled
- session hangs when a usb transfer fails
- build-aasdk loses protoc on reconfigure
- usb transaction error drops the whole session
- stop tears channels out from under io threads
- quick start then stop crashes the app
- a paired phone can become the car's speakers
- promises the code never kept
- tools fail silently without root
- fault injection ships to end users
- bundle only loads on the machine that built it

### Changed

- split the author's tooling into dev/

