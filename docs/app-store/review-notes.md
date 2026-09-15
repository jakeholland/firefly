# App Review notes — Firefly

For the reviewer, and for whoever fills in App Store Connect's "Notes"
field before submitting. Plain answers to the three things a first-time
reviewer needs and cannot get from the app icon alone.

## What this app needs

Firefly is a companion app for a piece of hardware: **a Firefly puck**,
a small Meshtastic radio. The full feature set — seeing your crew's
position, sending messages, starting or joining a crew — only works
connected to one over Bluetooth. Nothing in the app can be exercised
end-to-end without a puck in range.

We know App Review does not have one. That is what the demo is for.

## How to use the demo

On first launch, tap **Start a crew** (or **Join a crew**) from the
welcome screen, then **Try the demo** on the connect-your-puck step —
it sits next to "Don't have a puck yet?". You can also reach it later
from **More → Settings → Try the demo**.

The demo runs entirely on the device — no Bluetooth, no network, no
real radio. It seeds a small scripted crew (Taylor, Dana, Sam, Mo,
Camp), a couple of messages, and a live-looking signal reading, so
Find, Inbox, and Lineup all have real content to look at instead of
their (equally real, but empty) first-run states. A **DEMO** strip
stays on screen the whole time, on every screen, so it's never
mistaken for a live connection.

**Leave the demo** is in the same place — More → Settings — once
you're in it. It returns to the ordinary first-launch screen. Nothing
from the demo is saved anywhere: it runs in memory only and leaves no
trace in the app's real history, settings, or crew data.

If you have a Meshtastic-compatible radio on hand, "Don't have a puck
yet?" (same screen) explains what a puck is and what the app can and
cannot do without one — the demo is the alternative for reviewing
without one, not a requirement to use it.

## Permissions

Firefly requests permissions only when a screen actually needs them —
never at launch, and never during the demo:

- **Bluetooth** is requested the first time you tap Connect (or a demo
  step drives it — the demo itself never touches Bluetooth at all).
- **Location** is requested when you turn on "Share my location", so
  your crew can find you. It is optional; the app works without it.
- **Notifications** are requested the first time a background link
  connects, so we can tell you about a message while the app isn't in
  front. Also optional.
- **Camera** is requested only when you tap the QR scanner to join a
  crew.

A plain launch with no interaction — including the entire demo walk
above — requests none of these.
