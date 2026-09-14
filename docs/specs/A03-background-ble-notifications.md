# A03 · background BLE, reconnection and local notifications

> **A-series.** Like A01/A02 this is a phone spec, and it follows the
> same rules: acceptance criteria become test names (`A03_AC3_...`),
> unknowns are represented rather than papered over, and anything cut is
> cut out loud. Where a claim can only be settled by a real iPhone, this
> spec says so rather than asserting it.

## Why this spec exists

A01's M2 shipped "background BLE" and it is real work — CoreBluetooth
state restoration, reconnect-on-loss, a bounded handshake retry, a
20-second rediscovery backstop, all of it bench-verified against a
Heltec. But M2's own acceptance criterion is *thirty minutes with the
screen off*. Lost Lands is **three days**, and the gap between those two
numbers is where every interesting failure lives: a phone that gets
jettisoned from memory overnight, a radio whose battery dies at 3am, a
Bluetooth toggle flicked in Control Centre, an aeroplane-mode round
trip, a friend sending a FLARE while the phone is in a pocket and the
app has not been foregrounded since noon.

There is a second, blunter reason. The feature is **off by default**
(`SettingsStore.backgroundConnectEnabled` reads `UserDefaults.bool`,
which is `false`, and `SettingsStoreTests
.testBackgroundConnectDefaultsFalseAndRoundTrips` pins that) — so unless
Bailey finds a toggle in Settings > CONNECTIVITY, none of the M2
machinery runs at all. Backgrounding disconnects the radio, cancels the
notification subscription, and the phone goes deaf. That is the single
highest-value line in this document.

**What this spec is not.** It is not a push-notification spec (there is
no server, and A01 cut push explicitly). It is not a rewrite of
`BLETransport` — most of what follows is additive, and the parts that
are corrections are named as corrections.

## Goals

1. **Three days, not thirty minutes.** The link comes back on its own
   after: a brief pocket loss, a radio power cycle, a radio battery
   death and recharge hours later, Bluetooth toggled off and on, a phone
   reboot, and iOS jettisoning the app from memory.
2. **The phone tells Bailey something happened.** A FLARE, a DM and a
   crew message that arrive while the app is not on screen produce a
   local notification with the right urgency — a FLARE cutting through a
   Focus mode, a crew message not waking anyone at 3am.
3. **Honest status.** One line, in plain language, that says what is
   actually true about the background connection — never "connected"
   when the OS has suspended us with no link.
4. **Battery that survives the weekend.** No unbounded scans, no 1 Hz
   pumps running against a screen nobody can see, no polling where a
   pending connect will do.

**Non-goals:** push notifications / APNs; a notification service
extension; Live Activities or Dynamic Island; a watch app; background
location beyond what A01 already does for the GPS uplink; notifications
for anything the radio did not actually deliver.

## 1. iOS facts this design rests on

Every design decision in §3 cites a line here. Facts are marked
**[Apple]** (documented by Apple) or **[community]** (widely reported
developer behaviour that Apple does not document) — the distinction
matters, because a design that leans on an undocumented behaviour needs
a backstop, which is exactly the lesson PR #279 already learned the hard
way on the bench.

### 1.1 `bluetooth-central` buys wake-ups, not runtime

- The mode means the system will "launch or resume the app, in the
  background, and afford it time to process any related events" — there
  is no continuous runtime, and the app is re-suspended between events.
  **[Apple]**
  (https://developer.apple.com/documentation/xcode/configuring-background-execution-modes)
- While backgrounded the app "can still discover and connect to
  peripherals, and explore and interact with peripheral data."
  **[Apple]**
  (https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html)
- `NSBluetoothAlwaysUsageDescription` is required. **[Apple]**
  (https://developer.apple.com/documentation/bundleresources/information-property-list/nsbluetoothalwaysusagedescription)
  Firefly has it (`Info.plist:35`).
- App Review hooks this on guideline 2.5.4 — "Multitasking apps may only
  use background services for their intended purposes". **[Apple]**
  (https://developer.apple.com/app-store/review/guidelines/)

**How long a wake lasts is not documented by Apple.** The widely-cited
developer figure is ~10 s per BLE wake. **[community]**
(https://developer.apple.com/forums/thread/114555). Treat it as a
budget, not a guarantee; exceeding CPU limits in the background is a
documented termination cause **[Apple]**
(https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/WorkLessInTheBackground.html).

### 1.2 State restoration — and the iOS 26 rule that changes the plan

- Opt in with a restore identifier that "must be identical across
  executions of the app." **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanageroptionrestoreidentifierkey)
  Firefly's is a fixed literal (`BLETransport.swift:833`) — correct.
- **Scene-based apps:** "In scene-based apps that adopt
  `UISceneDelegate`, `launchOptions` is always `nil` on launch, so
  `UIApplicationLaunchOptionsBluetoothCentralsKey` is not available…
  Persist the UID yourself… and pass it to
  `CBCentralManager(delegate:queue:options:)` **on every launch**."
  **[Apple]** (same page, and
  https://developer.apple.com/documentation/corebluetooth/central-manager-state-restoration-options).
  This is the documented basis for §3.1: the manager must be constructed
  on every launch, early, by us — the system will not hand it to us.
- `willRestoreState` is "the *first* method … invoked when your app is
  relaunched into the background", before `didUpdateState`. **[Apple]**
  (Programming Guide, link above)
- Restore keys: `CBCentralManagerRestoredStatePeripheralsKey`,
  `…ScanServicesKey`, `…ScanOptionsKey`. The dictionary can be sparse and
  "your app is responsible for restoring its previous state." **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanagerdelegate/centralmanager(_:willrestorestate:))
- The system preserves: the services being scanned for (and scan
  options), the peripherals connected or being connected to, and the
  characteristics subscribed to. **[Apple]** (Programming Guide)
- Restored `CBPeripheral` objects arrive with a nil delegate and must be
  retained, because "deallocating `peripheral` also implicitly calls
  `cancelPeripheralConnection(_:)`" **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/connect(_:options:));
  the nil-delegate detail is **[community]**. Firefly already reassigns
  the delegate and retains (`BLETransport.swift:1213`).

**TN3115 — when iOS actually relaunches you** **[Apple]**
(https://developer.apple.com/documentation/technotes/tn3115-bluetooth-state-restoration-app-relaunch-rules):

| State | Relaunched? |
|---|---|
| App suspended in memory | Activated, no relaunch needed |
| App removed from memory (jetsam) | **Yes** |
| App crashed | **Yes** |
| **Force-quit by the user** (swipe up) | **No** |
| Bluetooth power toggled **in Settings** | **No** |
| **Control Centre** Bluetooth button toggled | **Yes** |
| Airplane Mode toggled | **Yes** (if it toggles Bluetooth) |
| Device restarted | **Yes**, but not until the first unlock after restart |

And the gate: the app is relaunched "**if and only if** it's waiting for
a specific Bluetooth event or action (like scanning, connecting, or a
subscribed notification characteristic) … and the corresponding
Bluetooth event has occurred." **[Apple]**

> **⚠ The iOS 26 change, and it lands on this festival.** TN3115 note 5
> reads: "**Starting in iOS 26 and iPadOS 26, only apps that use
> AccessorySetupKit to setup Bluetooth accessories will be
> relaunched.**" **[Apple]** It is attached to the force-quit and
> Control-Centre-toggle rows. Firefly does not use AccessorySetupKit
> (introduced iOS 18,
> https://developer.apple.com/videos/play/wwdc2024/10203/). The
> conservative reading — and the one this spec designs against — is that
> on an iOS 26 phone, **a force-quit is unrecoverable until the user
> opens the app again**, and a Control Centre Bluetooth toggle may be
> too. Exactly which rows note 5 narrows is ambiguous in Apple's own
> wording and is marked **[unverified]**; §6 protocol P5 measures it on
> Jake's actual phone rather than guessing, and §3.13 says what we do
> either way.

### 1.3 A pending connect is the cheapest thing we have

- "**Attempts to connect to a peripheral don't time out.**" **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/connect(_:options:))
- A pending connect is one of TN3115's named wake sources, and
  `willRestoreState` exists "to service active or **pending**
  connections and scans that were in progress when your app stopped."
  **[Apple]** (willRestoreState page)
- Practical: branch on each restored peripheral's `state`; `.connecting`
  means the pending connect is still live. **[community]** — which is
  exactly what `BLETransport.handleWillRestoreState` already does
  (`:1224`).

### 1.4 iOS 17 auto-reconnect

- `CBConnectPeripheralOptionEnableAutoReconnect`: "After a peripheral
  device connects, this setting enables the system to initiate a
  connection to the peer device automatically when the link drops. The
  system uses
  `centralManager(_:didDisconnectPeripheral:timestamp:isReconnecting:error:)`
  to notify the caller about the disconnection." iOS 17.0+. **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbconnectperipheraloptionenableautoreconnect)
- `isReconnecting` — "A `Bool` indicating whether the central manager
  **will automatically attempt to reconnect** to the peripheral."
  `timestamp` — "the specific point in time when the disconnection
  occurred", which matters because the disconnect may have happened
  while the app was suspended. **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanagerdelegate/centralmanager(_:diddisconnectperipheral:timestamp:isreconnecting:error:))
- `isReconnecting == true` → do not call `connect()`; the system owns
  the retry. `false` → we must re-issue. **[community]** for the
  sequencing; the semantics are **[Apple]**.
- Apple documents **no** retry count, backoff or maximum duration for
  system auto-reconnect — **[unverified]**. This is why §3.6 keeps a
  backstop ladder instead of trusting it alone.
- Whether implementing the 5-argument delegate suppresses the legacy
  2-argument one is **[community]** (reported: yes). Implement both.
- There is no WWDC session on this; it is documentation-only.

### 1.5 Background scanning is a different, weaker instrument

- "your app **must explicitly scan for one or more services** by
  specifying them in the `serviceUUIDs` parameter"; a `nil` scan
  discovers nothing in the background. **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanager/scanforperipherals(withservices:options:))
  Firefly always passes `[serviceUUID]` (`BLETransport.swift:369`,
  `:599`, `:788`) — correct already.
- "`CBCentralManagerScanOptionAllowDuplicatesKey` … is ignored, and
  **multiple discoveries of an advertising peripheral are coalesced into
  a single discovery event**." **[Apple]** (Programming Guide)
- "If all apps that are scanning for peripherals are in the background,
  the interval at which your central device scans … increases. As a
  result, **it may take longer to discover an advertising peripheral**."
  **[Apple]** (same)
- Consequence for us: the §3.6 ladder gets **one** `didDiscover` per
  peripheral per scan window, and discovery latency is unbounded. The
  ladder must not assume a sighting will arrive promptly, and must never
  use RSSI while backgrounded.

### 1.6 The power alert

- `CBCentralManagerOptionShowPowerAlertKey` — "whether the system warns
  the user if the app instantiates the central manager when Bluetooth
  service isn't available… **If the key isn't specified, the default
  value is `true`.**" **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanageroptionshowpoweralertkey)
- Since §3.1 constructs a central on **every** launch, including
  background relaunches, the default would let iOS pop a system alert at
  arbitrary moments. Meshtastic-Apple sets it `false` for a measured UX
  bug (their `BLETransport.swift:118`, issue #2139). §3.5 does the same.

### 1.7 Timers do not run while suspended — this is load-bearing

- Apple documents no timer behaviour for suspended apps, but the
  behaviour is consistent and widely reported: `Timer`,
  `DispatchQueue.asyncAfter`, and `Task.sleep` **do not fire while the
  process is suspended**; wall-clock passes, no code runs, and an
  overdue timer fires once (coalesced) on the next resume.
  **[community]**
- The design consequence is explicit in Apple's own API surface: the iOS
  17 `timestamp:` parameter exists precisely because the disconnect can
  have happened while you were frozen. **[Apple]** (§1.4)
- **This invalidates any design that schedules recovery with
  `Task.sleep`**, which is what `armReconnectFallback`
  (`BLETransport.swift:332`) does today. §3.6 is written as a
  clock-delta evaluated at each wake, not as a sleeping task.
- `beginBackgroundTask(withName:expirationHandler:)` buys extra time,
  must be balanced with `endBackgroundTask` ("If you don't call
  `endBackgroundTask(_:)` for each task before time expires, **the
  system kills the app**"), should be requested at the *top* of a
  callback because the assertion is granted asynchronously, and is safe
  to call off the main thread. **[Apple]**
  (https://developer.apple.com/documentation/uikit/uiapplication/beginbackgroundtask(withname:expirationhandler:))
  It is reported not to reliably extend execution while the device is
  locked with the screen off. **[community]**
  (https://developer.apple.com/forums/thread/115362)

### 1.8 Apple's own battery guidance

- Stop scanning as soon as you have what you need; discover only the
  services and characteristics you name; **subscribe rather than poll**;
  disconnect when done. **[Apple]**
  (https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/BestPracticesForInteractingWithARemotePeripheralDevice/BestPracticesForInteractingWithARemotePeripheralDevice.html)
- "Interacting with Bluetooth accessories" is named explicitly as a
  common cause of background energy waste, and "Your app shouldn't wait
  to be suspended by the system. It should begin winding down activity
  immediately once notified that state has changed." **[Apple]**
  (Energy Efficiency Guide, link in §1.1)

Firefly's FROMNUM-notify-driven drain (`MeshtasticBLE.swift`,
`FromRadioDrainPolicy`, and `BLETransport`'s three drain triggers)
already follows the "subscribe, don't poll" rule — it is the single best
thing about the current background design and nothing in §3 changes it.

### 1.9 Core Bluetooth and Swift 6

- `CBCentralManager(delegate:queue:)` with `nil` queue delivers callbacks
  on the main queue. **[Apple]**
  (https://developer.apple.com/documentation/corebluetooth/cbcentralmanager)
  Firefly passes `nil` (`BLETransport.swift:809`).
- `CBPeripheral`/`CBService`/`CBCharacteristic` are **not** `Sendable`.
  There is no Apple guidance on the correct Swift 6 pattern — the
  Developer Forums thread asking exactly this is unanswered.
  **[community]** (https://developer.apple.com/forums/thread/777145)
- Community consensus: extract plain values inside the delegate callback
  and send *those* across the isolation boundary, rather than the
  CoreBluetooth objects. Firefly's `CoreBluetoothCrossing` sends the
  objects themselves; A01's "Swift 6 strict concurrency" scope cut
  covers this, and A03 does not reopen it — but §3 must not make the
  situation worse, and the new delegate method follows the existing
  pattern rather than inventing a second one.

### 1.10 Local notifications

- `trigger: nil` means "deliver the notification right away", and an
  identifier that matches a previously delivered notification "alerts
  the user again, **replaces** the old notification with the new one,
  and places the new notification at the top of the list." **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/unnotificationrequest/init(identifier:content:trigger:))
  This is the mechanism §3.11.3's derived identifiers use for dedupe.
- Interruption levels **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/unnotificationinterruptionlevel):
  - `.passive` — "adds the notification to the notification list without
    lighting up the screen or playing a sound."
  - `.active` — "presents the notification immediately, lights up the
    screen, and can play a sound." (the default)
  - `.timeSensitive` — "breaks through system notification controls" —
    i.e. Focus and Notification Summary — and "**The user can turn off
    the ability for time sensitive notification interruptions.**"
  - `.critical` — bypasses the mute switch; needs an Apple-approved
    entitlement, effectively granted only to medical/safety apps.
    **[community]** for the approval gate.
- `.timeSensitive` requires the **Time Sensitive Notifications**
  capability / `com.apple.developer.usernotifications.time-sensitive`.
  **[community]** for the exact key and Xcode flow — Apple's own
  entitlement reference page for this key 404s, which is worth knowing
  before someone spends an hour looking for it.
- **`.timeSensitive` as a `UNAuthorizationOptions` case is deprecated.**
  Use the entitlement plus `content.interruptionLevel`. **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/unauthorizationoptions)
- Content properties that matter here: `threadIdentifier` ("The
  identifier that groups related notifications"), `categoryIdentifier`,
  `userInfo`, `targetContentIdentifier` ("The value your app uses to
  determine which scene to display"), `relevanceScore`, `badge`, `sound`.
  **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/unmutablenotificationcontent)
- Categories must be registered at launch with
  `setNotificationCategories`, action identifiers must be unique **across
  all categories**, and "If you do not implement
  [`didReceive`], your app never responds to custom actions."
  **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/declaring-your-actionable-notification-types)
  **[community]:** re-register categories on every launch, background
  relaunches included.
- Permission: "Make the request in a context that helps people
  understand why your app needs authorization… Sending the request in
  context provides a better experience than automatically requesting
  authorization on first launch", and "**Always check your app's
  authorization status before scheduling local notifications.**"
  **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/asking-permission-to-use-notifications)
- Provisional authorization delivers "**quietly** — they don't interrupt
  the person with a sound or banner, or appear on the lock screen."
  **[Apple]** (same page) — which is why §3.11.5 refuses it for a FLARE.
- Foreground: if `willPresent` is implemented but returns nothing, "the
  system behaves as if you had passed
  `UNNotificationPresentationOptionNone`" — an implemented-but-lazy
  delegate silently swallows foreground notifications. **[Apple]**
  (https://developer.apple.com/documentation/usernotifications/unusernotificationcenterdelegate/usernotificationcenter(_:willpresent:withcompletionhandler:))
- There is a hard limit of **64 pending** notification requests per app —
  stated by an Apple engineer on the forums rather than in the reference
  docs. **[Apple, forum]**
  (https://developer.apple.com/forums/thread/811171). A `nil`-trigger
  notification is delivered immediately rather than queued, so this is
  not expected to bind on us **[community]/[unverified]** — but §3.11.3's
  dedupe keeps the count low regardless.
- No documented delivery-rate limit for immediate local notifications.
  **[unverified]** Self-imposed debouncing is the design position, not
  reliance on the system.

## 2. Current-state audit

Read against `origin/main` @ `dbbc79b`. Every row names a file:line in
this repo. "Works" is not a compliment here — it means the code does
what it claims; several rows that work are still wrong for three days.

### 2.1 What is genuinely there

| Claim | Where | Verdict |
|---|---|---|
| `bluetooth-central` background mode declared | `app/Firefly/Resources/Info.plist:56` | **Correct.** `location` is there too, for A01's GPS uplink. |
| A **fixed** restore identifier is set | `BLETransport.swift:831` — `com.jakeholland.Firefly.ble-central`, iOS only, `#else [:]` on macOS | **Correct**, and fixed rather than generated, which is the part that matters. |
| `willRestoreState` is implemented and branches on the restored peripheral's own state | `BLEDelegateBridge.swift:89` → `BLETransport.swift:1207` | **Correct shape.** `.connected` → rediscover services; `.connecting` → record as pending; else → `issueConnect`. |
| Reconnect-on-loss re-arms a *pending* connect, not a poll | `BLETransport.swift:995` | **Correct**, and this is the right primitive (§1). |
| At most one native `central.connect()` per peripheral | `BLETransport.swift:240`, `:250` | **Correct**, and pinned by `BLEContractTests` with no radio. |
| A bounded rediscovery backstop for a peripheral that cold-booted | `BLETransport.swift:332`, `:358` | **Correct reasoning** (PR #279 bench evidence), **wrong bound** — see 2.2.6. |
| Bounded exponential handshake retry after a transport reconnect | `MeshtasticClient.swift:294` (6 attempts, 2/4/8/16/32 s, 60 s cap), loop at `:1309` | **Correct.** |
| `.reconnecting(attempt:)` published the instant a loss is seen, not only once recovery succeeds | `MeshtasticClient.swift:1535` | **Correct** — PR #279's own fix, and an honesty win. |
| A lost bond is terminal rather than retried forever | `BLETransport.swift:51`, `:995` | **Correct.** |
| Local notifications exist at all, and are real code on both platforms | `FireflyModel/NotificationSending.swift:50` | **Partially** — see 2.3. |

### 2.2 BLE — what is missing or wrong

1. **Nothing creates a `CBCentralManager` at launch, so state
   restoration probably never fires.**
   `BLETransport.ensureCentralManagerExists()`
   (`BLETransport.swift:807`) is called from exactly two places:
   `connect()` (`:461`) and `scan()` (`:782`). The app has **no**
   `UIApplicationDelegateAdaptor` and no
   `didFinishLaunchingWithOptions` anywhere (`grep` across `app/`:
   zero hits); `AppGraph.start()` — which is what eventually calls
   `client.connect()` — runs from a SwiftUI `.task` attached to
   `RootView` (`FireflyApp.swift:273`). iOS delivers `willRestoreState`
   during the launch cycle, to a manager that already exists with the
   matching restore identifier. A manager that is constructed later,
   from a view's `.task`, is a **new** manager. The restoration code at
   `BLETransport.swift:1207` is, as far as this audit can establish
   without a device, unreachable in the one scenario it was written for.
   PR #272's own body says the same thing in different words: state
   restoration "needs real hardware to verify" and was not verified.
   **Testable only on an iPhone** (§6, protocol P3).

2. **Even if restoration fired, no client is listening.**
   `MeshtasticClient.receiveTask` — the only reader of
   `transport.events()` — is created inside `connect()`
   (`MeshtasticClient.swift:406`). A restored session completing through
   `completeConnect(throwing: nil)` yields `.ready` into a hub
   (`BLETransport.swift:1186`) that nobody has subscribed to;
   `hasCompletedInitialConnect` is `false`, so even the
   `consumeTransportEvents` `.ready` branch (`:1504`) would not start a
   handshake. FROMRADIO bytes would be drained and dropped on the floor.
   Restoration without a resumed client is restoration that changes
   nothing a user can see. **Loopback-testable.**

3. **Bluetooth off → on is a dead end.** `handleCentralStateUpdate`
   (`BLETransport.swift:899`) opens with
   `guard !poweredOnContinuations.isEmpty else { return }` — with no
   caller parked in `waitForPoweredOn()`, the entire callback is a
   no-op. So when the user flicks Bluetooth off in Control Centre and
   back on (or takes a flight, or lets iOS reset the stack), nothing
   re-issues a connect. Worse, `.poweredOff` never clears `peripheral`,
   `pendingConnectPeripheralID` or the characteristic references, so a
   later legitimate connect for that same identifier can be swallowed by
   `issueConnect`'s own guard (`:251`). **Loopback/unit-testable** as a
   pure state-machine decision; the CoreBluetooth half needs an iPhone.

4. **No `CBConnectPeripheralOptionEnableAutoReconnect`.** Every connect
   is `options: nil` (`BLETransport.swift:270`, `:570`). The deployment
   floor is iOS 17, so this is available and free.

5. **The legacy disconnect delegate only.** `BLEDelegateBridge.swift:117`
   implements `didDisconnectPeripheral(_:error:)` and not the iOS 17
   `didDisconnectPeripheral(_:timestamp:isReconnecting:error:)`. Without
   it there is no way to tell "the system is already reconnecting for
   us" from "we are on our own", which is precisely the decision
   `handleDisconnected` (`:973`) is making by hand.

6. **The rediscovery fallback scan is unbounded.**
   `runReconnectFallbackScan` (`BLETransport.swift:358`) starts
   `scanForPeripherals` and sets `isFallbackScanning = true`. The only
   three things that ever stop it are: rediscovering that exact
   peripheral (`:940`), a connect chain completing (`:1164`), and an
   explicit user `disconnect()` (`:619`). A radio whose battery died at
   2am leaves the phone scanning continuously until morning. This
   directly contradicts `endFallbackScan()`'s own doc comment — "a
   continuous BLE scan is one of the most expensive things an iPhone can
   be asked to do, and this app's whole premise is a phone that lasts
   three days in a field" (`:305`) — which was written about a *different*
   leak on the same flag.

7. **No attempt ceiling or backoff on the background reconnect path.**
   `connectRetryLimit = 2` and the 5 s/90 s connect timeouts
   (`BLETransport.swift:416`) apply only to the foreground `connect()`
   loop (`:468`). `handleDisconnected` (`:995`) re-arms a pending
   connect plus one fallback scan with no counter and no ladder.

8. **No liveness watchdog on BLE.** The heartbeat is explicitly
   serial/TCP-only — "BLE gets its liveness from the link itself"
   (`MeshtasticClient.swift:288`). That is true for a link that *drops*,
   and false for a radio that hangs with the GATT connection still up:
   the app shows `CONNECTED`, uptime keeps counting, and nothing has
   arrived for six hours. For a three-day deployment this is the failure
   mode most likely to be discovered by a person rather than by the app.

9. **No `CBCentralManagerOptionShowPowerAlertKey`, no
   `registerForConnectionEvents`.** Neither appears anywhere in the tree.
   Both are optional; §3 says which one is worth adding and which is not.

### 2.3 Notifications — what is missing or wrong

10. **The graph thinks it is foregrounded when it is not.**
    `AppGraph.isForegrounded` is initialised `true`
    (`AppGraph.swift:139`) and is only ever updated by
    `.onChange(of: scenePhase)` (`FireflyApp.swift:253`). `onChange` does
    not fire for an initial value. Any launch that begins in the
    background — a CoreBluetooth relaunch above all — leaves the graph
    believing someone is looking at the screen. `handleInboundFlare`
    (`AppGraph+M2Protocol.swift:66`) then takes the **takeover** branch
    and renders a full-screen view to nobody instead of posting a
    notification, and `observeIncomingTextsForNotifications`
    (`:234`) `continue`s past every arriving message. Zero notifications
    on exactly the path notifications exist for.
    **Unit-testable today** against `AppGraph`.

11. **The first notification of the festival is always dropped.**
    `UNNotificationSending.post` requests authorization lazily on first
    need (`NotificationSending.swift:77`), and the only callers are the
    two background branches. So the first FLARE calls
    `requestAuthorization` while the app is backgrounded, iOS cannot
    present the prompt, `notificationSettings().authorizationStatus`
    reads `.notDetermined`, and `post` returns silently (`:85`). The
    alert is lost and the prompt surfaces later, out of context.

12. **No interruption level.** Nothing in the tree sets
    `interruptionLevel`, so every notification defaults to `.active` —
    which means **a FLARE is silenced by Sleep Focus, Do Not Disturb or
    any custom Focus**. A FLARE is the one message in this product whose
    entire purpose is to interrupt. The entitlement that would fix it
    (`com.apple.developer.usernotifications.time-sensitive`) is also
    absent: the iOS target signs with `Firefly.entitlements`
    (`app/project.yml:138`), which contains only macOS App Sandbox keys.

13. **No thread identifier, no category, no actions, no `userInfo`.**
    `NotificationSending.swift:89` builds title + body + default sound
    and nothing else. Consequences: notifications do not group per
    sender; tapping one just opens the app wherever it was, never the
    thread or Find; there is no "Find them" or "Reply" action.

14. **No dedupe.** The request identifier is `UUID().uuidString`
    (`:94`), so a mesh retransmit that the client surfaces twice
    notifies twice.

15. **RALLY never notifies.** `handleInboundRally`
    (`AppGraph+M2Protocol.swift:108`) pushes a feed item and stops.
    "Meet here" is the second most time-critical packet the product has.

16. **DM and crew broadcast are indistinguishable.**
    `observeIncomingTextsForNotifications` (`:234`) posts the same
    `postMessage(senderName:preview:)` for both. A crew channel with
    eight people on it at 2am is a phone that buzzes all night.

17. **No badge, no quiet hours.** Authorization asks for
    `[.alert, .sound]` only (`:80`). The puck has a quiet-hours policy
    with FLARE explicitly exempt (`firmware/core/include/ff_sound.h`);
    the phone has no analogue at all.

### 2.4 Lifecycle and battery

18. **`backgroundConnectEnabled` defaults to `false`.**
    `SettingsStore.swift:116` reads `rawBool` → `UserDefaults.bool` →
    `false`, and `SettingsStoreTests.swift:97` pins it. With the default
    in place, `handleScenePhaseChange(.background)`
    (`AppGraph.swift:515`) calls `stop()`, which disconnects the client
    and cancels the notification subscription (`:562`). Out of the box,
    Firefly goes deaf the moment the screen locks. **This is the
    highest-value single-line change in this spec.**

19. **With the setting on, backgrounding does nothing at all.**
    `AppGraph.swift:523` returns early. The 1 Hz `tickLoop`
    (`:320`) keeps running, and if Find's Radar segment was the visible
    one, `RadarViewModel.recomputeLoop` keeps recomputing radar geometry
    against a screen nobody can see — deliberately, pinned by
    `AppGraphViewModelLifecycleTests
    .testBackgroundingWithBackgroundConnectOnLeavesRadarRunning` and
    flagged as an interpretation call in PR #298. In practice iOS
    suspends the process between BLE events so these loops only burn the
    wake windows, but they burn *every* wake window, and they are doing
    UI work in a process that has no UI on screen. PR #294 fixed exactly
    this shape for Map (`MapTabView` gained a `scenePhase` observer) and
    for the `backgroundConnectEnabled == false` path; the
    `== true` path was left as-is.

20. **Nothing catches up after a long gap.** There is no notion of "the
    app was away for six hours, tell me what I missed" — no backfill
    request to the radio, no "you may have missed messages" marker, and
    `want_config` rebuilds the nodeDB rather than replaying traffic. The
    honest position (§3.8) is to say so on screen rather than to imply
    completeness.

## 3. Target design

Additive wherever possible. Each subsection names the audit item it
closes.

### 3.1 A `CBCentralManager` that exists before iOS wants to talk to it

*(closes 2.2.1, 2.2.2)*

Two changes, both idempotent, so whichever runs first wins and the
second is a no-op:

1. **`BLETransport.prepareForRestoration()`** — a new `public` method
   that does nothing but call the existing
   `ensureCentralManagerExists()` (`BLETransport.swift:807`). The guard
   there (`guard central == nil`) already makes it safe to call any
   number of times.
2. **An `AppDelegate` via `UIApplicationDelegateAdaptor`** whose
   `application(_:didFinishLaunchingWithOptions:)` calls
   `prepareForRestoration()` on the live transport and kicks
   `AppGraph.start()` (already guarded by its own `started` flag,
   `AppGraph.swift:283`). This is the hook iOS actually guarantees runs
   on a background relaunch; a SwiftUI `.task` is not.

`FireflyApp.init()` additionally calls `prepareForRestoration()`, as a
belt-and-braces second path for the ordering question SwiftUI does not
document. Neither path may call `connect()` — restoration must be
allowed to adopt the session rather than race a fresh connect.

**The client has to be listening too.** `MeshtasticClient` moves its
transport-event subscription out of `connect()`
(`MeshtasticClient.swift:406`) into a new
`beginListening()` — called once by `AppGraph.start()`, before any
connect. `consumeTransportEvents`'s `.ready` branch (`:1504`) then drops
its `hasCompletedInitialConnect` gate in favour of "is a
`connect()` continuation in flight": a `.ready` that nobody is awaiting
is, by definition, a restored or reconnected session and must run the
handshake. This is the one change in this spec that touches a path A01
M1 relies on, so it is S1's riskiest edit and gets its own loopback
test.

On `willRestoreState` with a `.connected` peripheral, the handshake is
**not** re-run from scratch: services are rediscovered
(`BLETransport.swift:1232`) and the client issues `want_config` only
because *this process* has no nodeDB. Meshtastic-Apple skips both config
and database on an adopted `.connected` restore
(`BLETransport.swift:600-604` in their tree); we cannot, because A01
deliberately does not persist the nodeDB (A01, "Why the nodeDB is not
persisted"). That cost — one `want_config` per background relaunch — is
accepted and stated, not hidden.

**A `restoreInProgress` flag**, borrowed from Meshtastic-Apple,
suppresses `handleDiscovered`'s fallback-scan branch and any node-picker
scan while a restore is being adopted, so the two cannot race.

### 3.2 The graph must not assume it is on screen

*(closes 2.3.10)*

`AppGraph.isForegrounded` flips its default from `true`
(`AppGraph.swift:139`) to **`false`**, and is set `true` only by an
observed `.active` scene phase. This is the honest default and it fails
safe in the direction that matters: an inbound FLARE on a path where
nothing has told us we are visible posts a notification rather than
rendering a takeover to an empty screen.

`FireflyApp` additionally seeds the value once at scene attach (from the
initial `scenePhase`, not only from `.onChange`), and the `AppDelegate`
seeds it from `UIApplication.shared.applicationState` on launch — the
only signal available during a background relaunch, before any scene
exists.

### 3.3 The setting defaults to on

*(closes 2.4.18)*

`SettingsStore.backgroundConnectEnabled` (`SettingsStore.swift:116`)
becomes a three-state read: unset → **`true`**; explicitly set → that
value. `SettingsStoreTests.testBackgroundConnectDefaultsFalseAndRoundTrips`
is renamed and inverted rather than deleted, so the change is visible in
the diff of a test whose name states the product decision.

Justification, stated plainly: this app's entire purpose is a phone in a
pocket at a festival. A default that disconnects on screen-lock is a
default that makes the product not work, and "the user can turn it on"
is not a defence when the user is Bailey, at night, in a field. The
toggle stays — someone who wants the radio off when the app is closed
can still have that — and the Settings subtitle
(`SettingsScreen.swift:253`) keeps saying exactly what each position
does.

### 3.4 Pending connect, plus iOS 17 auto-reconnect

*(closes 2.2.4, 2.2.5)*

`issueConnect(_:)` (`BLETransport.swift:250`) passes
`[CBConnectPeripheralOptionEnableAutoReconnect: true]` on iOS 17+
(`#if os(iOS)`; macOS keeps `nil`). The pending connect stays the
primary mechanism — it is the thing that costs nothing while the app is
suspended — and the option asks the system to keep re-establishing the
link on our behalf after a drop, which is strictly more than a pending
connect does on its own.

`BLEDelegateBridge` gains
`centralManager(_:didDisconnectPeripheral:timestamp:isReconnecting:error:)`
alongside the legacy callback. iOS calls the new one when it is
implemented, so the legacy method stays only for macOS and as a
fallback. The new signal changes exactly one decision in
`handleDisconnected` (`BLETransport.swift:973`):

| `isReconnecting` | What we do |
|---|---|
| `true` | Publish `.disconnected` + `.reconnecting(attempt: 1)`; **do not** call `issueConnect`, **do not** arm a fallback scan. The system is already on it; a second connect is duplicated radio work. |
| `false` | Exactly today's behaviour: `issueConnect` + the (now bounded, §3.6) fallback ladder. |

`timestamp` is recorded and surfaced as "last heard" (§3.9) — it is a
real observation, which is the only kind of number this project
displays.

### 3.5 Bluetooth off and back on

*(closes 2.2.3)*

`handleCentralStateUpdate` (`BLETransport.swift:899`) loses its
"nobody is waiting, so do nothing" guard. The guard moves to only the
part that actually needs it (resuming `poweredOnContinuations`), and the
method gains real state handling:

- **`.poweredOff`** — cancel the fallback ladder, `endFallbackScan()`,
  drop `peripheral`'s characteristic references, clear
  `pendingConnectPeripheralID` (CoreBluetooth has invalidated the
  pending connect), fail every outstanding continuation, and publish
  `.disconnected(reason: "bluetooth-off")`. **`shouldAutoReconnect` is
  deliberately preserved** — the user turning Bluetooth off is not the
  user asking us never to reconnect.
- **`.poweredOn`** — if `shouldAutoReconnect` and a
  `preferredPeripheralID` exists, `retrievePeripherals(withIdentifiers:)`
  and `issueConnect` the result. If the identifier no longer resolves,
  arm the §3.6 ladder instead.
- **`.unauthorized` / `.unsupported`** — terminal, as today, and now
  also published as a distinct link reason so §3.10's status line can
  say "Bluetooth access is off for Firefly" rather than a generic
  failure.
- **`.resetting`** — treated as a transient loss, not a terminal one:
  publish `.reconnecting(attempt: 1)` and wait for the next transition.

The decision table above is extracted as a pure function,
`BLETransport.powerStateAction(for:shouldAutoReconnect:hasPreferred:)`,
so `BLEContractTests` can pin every row with no `CBCentralManager` —
the same shape `shouldIssueConnect` and `shouldRunReconnectFallbackScan`
already use.

`CBCentralManagerOptionShowPowerAlertKey: false` is **added**, following
Meshtastic-Apple (their `BLETransport.swift:118`, where the system alert
blipped `scenePhase` and produced a dismiss/reappear loop — their issue
#2139). Firefly's Connect screen already says Bluetooth is off in its
own words; a system alert on top of that is a second, worse voice.

`registerForConnectionEvents` is **not** adopted. It solves "wake me
when a peripheral I care about connects, even if I did not connect it" —
useful for accessories paired outside the app, which a Meshtastic radio
is not. Cut out loud rather than left as a maybe.

### 3.6 A bounded rediscovery ladder

*(closes 2.2.6, 2.2.7)*

`armReconnectFallback(for:)` (`BLETransport.swift:332`) becomes a
ladder with a **scan window** and a **retry interval**, both bounded:

| Attempt | Fires after | Scan window |
|---|---|---|
| 1 | 20 s (today's `reconnectFallbackDelay`, a Heltec's own boot time) | 30 s |
| 2 | 1 min | 30 s |
| 3 | 2 min | 30 s |
| 4 | 5 min | 30 s |
| 5 | 10 min | 30 s |
| 6+ | 15 min, capped, indefinitely | 30 s |

**The ladder is a clock, not a sleeping task.** This is the correction
§1.7 forces: `armReconnectFallback`'s `Task.sleep`
(`BLETransport.swift:335`) does not run while the process is suspended,
so today's 20-second backstop fires 20 seconds after the app is *next
woken*, not 20 seconds after the loss. The ladder therefore stores
`disconnectedAt` (from the iOS 17 `timestamp:`, §3.4 — a real
observation, not our own clock at callback time) plus an attempt count,
and **evaluates `Date.now - disconnectedAt` against the table at every
opportunity the OS actually gives us**: each CoreBluetooth delegate
callback, each `centralManagerDidUpdateState`, and each foreground
transition. A `Task.sleep` is kept only as an opportunistic nudge for
the case where the app happens to still be running — never as the thing
correctness depends on.

Rules:

- A scan window **always** ends, whether or not the peripheral was seen
  — closed by the same clock-delta evaluation, and by
  `endFallbackScan()` on any wake where the window has expired. This is
  the fix for 2.2.6 and it is the single most important battery line in
  this spec.
- The ladder never ends while `shouldAutoReconnect` is true — a radio
  that is dead overnight must still be found at breakfast — but its
  *duty cycle* falls to 30 s of scanning per 15 min, i.e. **3.3 %**.
- The pending `connect()` (with auto-reconnect, §3.4) stays armed the
  whole time. The ladder is a backstop for the case PR #279 measured, not
  a replacement.
- ±20 % jitter on every interval, so two phones that lost the same radio
  do not scan in lockstep.
- A scan window may yield **one** `didDiscover` for the peripheral, ever
  (§1.5 — duplicates are coalesced in the background), and may yield it
  late. The ladder therefore treats "no sighting in this window" as no
  information at all, never as evidence the radio is gone.
- Any `.poweredOff` cancels the ladder; `.poweredOn` restarts it at
  attempt 1.

The interval table is a pure function
(`BLETransport.reconnectLadderDelay(forAttempt:)`), pinned by tests,
exactly like `MeshtasticClient.handshakeRetryDelay(forAttempt:base:cap:)`
already is.

### 3.7 A radio that reboots, and a handshake that gives up

*(partly closes 2.2.8)*

`MeshtasticClient`'s bounded handshake retry
(`MeshtasticClient.swift:294`, 6 attempts) stays as it is for the
foreground case. One change for the background case: when the retry is
exhausted and `.failed` is published, the client no longer requires a
user tap to try again. Instead `.failed` arms **one** re-attempt on the
§3.6 ladder's next tick, so a node that took longer than ~1 minute to
boot recovers on its own. `.failed` remains honestly published — the
status line says "still trying", not "connected".

`FromRadio.rebooted` handling (`MeshtasticClient.swift:1591`) is
unchanged; it already restarts the handshake.

### 3.8 A link that is up and dead

*(closes 2.2.8)*

Firefly will not invent a heartbeat for BLE — Meshtastic-Apple sets
`requiresPeriodicHeartbeat = false` for BLE for the same reason we do,
and a 15-minute background write is not free. What it will do is stop
*claiming* liveness it has not observed:

- `MeshtasticClient` records **`lastInboundAt`** — the timestamp of the
  most recent byte from the radio, from any characteristic.
- The status line (§3.10) and Diagnostics render **"last heard N ago"**
  next to link state, never uptime alone. `Link uptime`
  (`DiagnosticsViewModel.swift:105`) measures how long we have believed
  we were connected; that is a different, weaker claim and is labelled as
  such.
- On **foreground only**, if `lastInboundAt` is older than 10 minutes,
  the app sends one `ToRadio.heartbeat` and expects any inbound within
  5 s. No answer → cycle the link (`disconnect` + connect) once, and say
  so. Foreground-only because that is when a user is present to benefit,
  and it costs nothing while suspended.

### 3.9 Coming back after a long gap

*(closes 2.4.20)*

Two honest behaviours, no fabrication:

1. **Catch-up is whatever the radio still has.** On foreground after
   more than 2 minutes away, the app does not invent a backfill request
   — Meshtastic has no "replay what I missed" API and `want_config`
   rebuilds the nodeDB, it does not replay traffic. What the app does is
   drain FROMRADIO (already automatic on every FROMNUM) and render what
   arrives.
2. **A gap marker.** If the link was **not** `.ready` for a contiguous
   span longer than 5 minutes, the Inbox inserts a non-message row:
   *"Firefly wasn't connected from 11:40 pm to 1:15 am. Anything sent
   then may not have reached you."* This is the same class of statement
   as A02 §6.3's presence words — it says what is unknown rather than
   implying completeness. It is stored in history like any other feed
   row so it survives a relaunch.

Radar/crew freshness needs no new work: `ff_crew` presence is already
age-driven, so a six-hour gap renders as `NO SIGNAL` on its own.

### 3.10 The "Background connection" status line

*(the honest-UI requirement)*

One row, on the Connect screen and repeated in Settings >
CONNECTIVITY under the toggle. It states what is true right now, in the
A02 §6.4 register — no jargon, no dBm, no node ids:

| Condition | Line |
|---|---|
| Setting off | **Off** — "Firefly disconnects when you leave the app." |
| On, link `.ready`, heard < 2 min ago | **On** — "Staying connected in your pocket. Heard your puck just now." |
| On, link `.ready`, heard 2–10 min ago | **On** — "Staying connected. Last heard 6 min ago." |
| On, link `.ready`, heard > 10 min ago | **On** — "Connected, but nothing heard for 12 min." |
| On, reconnecting | **On** — "Lost your puck. Still looking — last heard 40 min ago." |
| On, Bluetooth off | **Paused** — "Bluetooth is off. Turn it on to reach your puck." |
| On, Bluetooth denied to Firefly | **Paused** — "Firefly can't use Bluetooth. Turn it on in Settings." |
| On, notifications not allowed | **On** — "Staying connected, but Firefly can't alert you. Turn on notifications." |
| On, bond lost | **Stopped** — "Your puck forgot this phone. Forget it in iOS Settings > Bluetooth, then connect again." |

Rules: the word "connected" appears only when the link is `.ready`. Every
line that shows an age shows a *measured* age (`lastInboundAt`), and
renders `UNKNOWN` rather than a guess if there is none — the same rule
`DiagnosticsViewModel.uptimeLabel` (`:105`) already follows.

### 3.11 Local notifications

*(closes 2.3.11 – 2.3.17)*

#### 3.11.1 Which events, and at what urgency

| Event | Interruption level | Sound | Thread id | Category |
|---|---|---|---|---|
| **FLARE** from a paired crew member | `.timeSensitive` | `.default` | `flare` | `FLARE` |
| **RALLY** from a paired crew member | `.active` | `.default` | `rally` | `RALLY` |
| **DM** (text addressed to us) | `.active` | `.default` | `dm-<nodeNum>` | `MESSAGE` |
| **Crew message** (channel broadcast) | `.active`, or `.passive` during quiet hours (§3.11.4) | `.default` / none when passive | `crew` | `MESSAGE` |
| Link lost / reconnecting | **none** | — | — | — |

The last row is a deliberate cut: a notification about our own plumbing
is noise at a festival, and the §3.10 status line is where that belongs.
Radio battery telemetry is also cut — the M1 client seam exposes no
telemetry at all (`DiagnosticsViewModel`'s own UNKNOWN comment), and a
notification we cannot source is exactly the fabrication this repo
refuses.

`.timeSensitive` requires
`com.apple.developer.usernotifications.time-sensitive` in the iOS
entitlements and the matching capability on the App ID. The iOS target
currently signs with `Firefly.entitlements`
(`app/project.yml:138`), which holds only macOS App Sandbox keys — S2
adds a real iOS entitlements file. **If the entitlement is not granted,
the level silently degrades to `.active`**, so the app must not claim
otherwise: Diagnostics shows whether the time-sensitive level is
actually available.

`.critical` is **not** used. It needs a special Apple entitlement,
overrides the hardware mute switch, and Meshtastic-Apple ships the
entitlement with the code path unreachable — a middle state this spec
declines to copy.

#### 3.11.2 Wording

Plain language, per A02 §6.4. Names, never node ids; no "portnum", no
dBm, no `!02e5e3d4`.

| Event | Title | Body |
|---|---|---|
| FLARE | `Taylor needs you` | `They sent a flare. Tap to find them.` |
| FLARE, unknown name | `Someone needs you` | `A flare came in from your crew. Tap to find them.` |
| RALLY | `Taylor set a meeting spot` | `MY SPOT — 210 m NE of you` (distance only when both fixes are real; otherwise just the name) |
| DM | `Taylor` | the message text, truncated by iOS |
| Crew message | `Taylor · crew` | the message text |

"Someone" is the existing fallback (`AppGraph+M2Protocol.swift:73`) and
stays — it is honest about a name we do not have.

#### 3.11.3 Dedupe, grouping and tap

- **Identifier** is derived, never random: `flare-<from>-<startedAtMs>`,
  `rally-<from>-<packetId>`, `msg-<from>-<packetId>`. A repeat of the
  same packet replaces the existing notification instead of adding a
  second one — which is what `UUID().uuidString`
  (`NotificationSending.swift:94`) prevents today.
- **`threadIdentifier`** groups per conversation (table above), so a
  chatty crew channel is one stack, not forty banners.
- **`userInfo`** carries a deep link: `firefly://thread/crew`,
  `firefly://thread/dm/<nodeNum>`, `firefly://find/<nodeNum>` for a
  FLARE. `UNUserNotificationCenterDelegate.userNotificationCenter(_:didReceive:)`
  on the `AppDelegate` (§3.1) routes it into the existing tab/segment
  selection — a FLARE opens Find ▸ Radar with that member selected; a
  message opens its thread.
- **Categories and actions.** `FLARE` gets one action, **Find them**
  (foreground, same destination as the tap). `MESSAGE` gets **Reply**
  (`UNTextInputNotificationAction`) — but only once there is a
  background send path that can honestly report failure; until then S2
  ships `MESSAGE` with no actions rather than a Reply that can silently
  not send. Stated as the cut it is.
- **Badge.** Authorization adds `.badge`; the badge count is the number
  of unread conversations, cleared when the Inbox is opened.
- Delivered notifications for a conversation are **withdrawn** when that
  thread is opened (`removeDeliveredNotifications(withIdentifiers:)`),
  the one behaviour of Meshtastic-Apple's notification manager worth
  copying wholesale.

#### 3.11.4 Quiet hours

A Settings row, **off by default**, with a start and end time (default
suggestion 1 am – 9 am when first enabled). While inside the window:

- **FLARE always alerts**, at `.timeSensitive`, sound on. This mirrors
  the puck exactly — `ff_sound`'s quiet-hours allow-list contains the
  FLARE events and nothing else
  (`firmware/core/include/ff_sound.h`, S27 §"quiet hours exempts only
  the two FLARE events").
- **RALLY and DM** alert at `.active` with sound.
- **Crew messages** drop to `.passive` with no sound: they appear in
  Notification Centre and never light the screen.

The window is evaluated against the phone's own clock at post time, and
S18's wall-clock-trust rule applies — if the clock is not trusted, quiet
hours is treated as **off** rather than guessed at.

#### 3.11.5 Permission, asked at a moment that can answer

*(closes 2.3.11)*

`UNNotificationSending.requestAuthorizationIfNeeded`
(`NotificationSending.swift:77`) stops being reachable from the posting
path. Instead:

1. The first time the link reaches `.ready` **while the app is in the
   foreground**, the Connect screen shows a one-line explanation — "Let
   Firefly tell you when someone flares or messages you while your phone
   is in your pocket." — with a single button.
2. That button calls `requestAuthorization([.alert, .sound, .badge])`.
3. `post(...)` never requests authorization. If the status is
   `.notDetermined` or `.denied` it records the fact for §3.10's status
   line and returns — the same honest no-op as today, minus the
   swallowed first alert.

Provisional authorization (`.provisional`) is explicitly **not** used:
it delivers quietly to Notification Centre, which for a FLARE is worse
than asking.

### 3.12 Background-loop compliance

*(closes 2.4.19, and keeps PR #294/#298 honest)*

`AppGraph.handleScenePhaseChange(.background)` (`AppGraph.swift:515`)
stops being a no-op when `backgroundConnectEnabled` is on. It keeps the
link and every subscription, and additionally:

- Stops `RadarViewModel.recomputeLoop` and `MapViewModel.pinRefreshLoop`
  — UI pumps for screens nobody can see. `start()`'s existing
  `radarWasObservingAtStop` restore mechanism (`AppGraph.swift:312`)
  already exists for exactly this and is reused, so foregrounding puts
  back precisely what was running. This **supersedes**
  `AppGraphViewModelLifecycleTests
  .testBackgroundingWithBackgroundConnectOnLeavesRadarRunning`, which
  pinned the opposite; that test is rewritten, not deleted, and the PR
  body says so (it was flagged as an interpretation call in PR #298, and
  this is the interpretation changing on purpose).
- Drops the 1 Hz `tickLoop` (`AppGraph.swift:320`) to **30 s** while
  backgrounded. Its only jobs are `ff_feed_expire_pending_acks` (a
  5-minute window) and the FLARE takeover's auto-dismiss (which cannot
  be showing while backgrounded). One-second resolution buys nothing off
  screen.

Everything that must keep working in the background is event-driven —
FROMNUM notify → drain → decode → notify — which is precisely the shape
iOS's `bluetooth-central` wake budget is designed for. No timer in this
app is load-bearing for delivering a notification, and that is a
property S3 verifies rather than assumes.

### 3.13 The iOS 26 relaunch gate — say it, don't paper over it

*(from §1.2)*

TN3115 note 5 restricts force-quit and Control-Centre-toggle relaunch to
AccessorySetupKit apps on iOS 26+. Firefly is not one. Three
consequences, all of them product decisions rather than code:

1. **AccessorySetupKit is not adopted in this spec.** It would change
   how a radio is discovered and paired (a system sheet instead of
   Firefly's own node picker), which is an A01 Connect-screen rewrite and
   a `[api]` change to `NodeScanning`. It is the right long-term answer
   if P5 (§6) shows the gate really does bite, and it is filed as a
   follow-up with that evidence attached — not guessed at now, a week
   before the festival.
2. **First-run and the Connect screen must tell the truth about
   force-quit.** One line, once, where it is actionable: *"Don't swipe
   Firefly away in the app switcher — it can't reconnect on its own
   after that."* That is the user education TN3115 explicitly asks for
   ("it's important to educate the users of your app"), and it costs a
   sentence.
3. **The §3.10 status line must not claim background coverage it does
   not have.** If the app has been launched by a person rather than by
   CoreBluetooth after a force-quit, there is nothing to detect — so the
   honest move is the sentence in (2), shown before it matters, not a
   status line invented after the fact.

A Bluetooth toggle done **in Settings** never relaunches us (§1.2
table) — that case is covered only because the app is usually still
alive in memory, and §3.5 handles it when it is. Stated, not hidden.

### 3.14 Background task assertions around the notification path

*(from §1.7)*

The path FROMNUM notify → drain → decode → `UNUserNotificationCenter
.add` is asynchronous and crosses at least two actor hops. If iOS
re-suspends the process between the decode and the `add`, the
notification never posts and nothing anywhere reports a failure.

So: `AppGraph` wraps the inbound-packet handling that can produce a
notification in a task assertion — `beginBackgroundTask` taken at the
**top** of the handler (the assertion is granted asynchronously, §1.7),
`endBackgroundTask` in a `defer` on every path including the throwing
ones. Never held across a `Task.sleep`, and never left dangling: an
unbalanced assertion is a documented way to get the app killed.

`UIApplication` is not reachable from `FireflyModel` (the package has no
UIKit dependency — the same constraint `HapticSignaling` already works
around), so this arrives as a small protocol, `BackgroundTaskAsserting`,
with a `NoBackgroundTaskAsserting` default for tests and macOS and a
`UIKitBackgroundTaskAsserting` injected from the app target. The seam
also makes "did we actually take an assertion for this packet"
unit-testable, which a direct `UIApplication` call would not be.

Honest caveat, recorded here because it will be tempting to trust:
assertions are reported not to reliably extend execution while the
device is locked with the screen off (§1.7) — which is most of a
festival night. The assertion is worth taking because it is cheap and
helps when it works; it is **not** the reason the design works. The
reason the design works is that the whole path is event-driven and
short.

## 4. Battery budget
Numbers first, honesty immediately after: **nobody has measured
Firefly's background battery cost, on any phone, ever.** What follows is
a budget to design against and a protocol to measure it with (§6, P8),
not a claim about what the app does today.

### 4.1 What costs what

| Activity | Cost | Firefly's use |
|---|---|---|
| An idle BLE connection with a subscribed notify characteristic | Cheapest option available; the radio link is maintained by the controller, not the app | The steady state — FROMNUM notify, nothing polled (§1.8) |
| A pending `connect()` for an absent peripheral | Effectively free to the app; the controller does the work | Primary reconnect mechanism (§3.4) |
| Continuous `scanForPeripherals` | The expensive one — `endFallbackScan()`'s own comment (`BLETransport.swift:305`) is right | Bounded to 30 s windows on the §3.6 ladder |
| A background wake (~10 s budget, §1.1) | Small individually, unbounded in aggregate | One per FROMNUM notification |
| A 1 Hz UI recompute loop in a backgrounded process | Pure waste | Stopped (§3.12) |

### 4.2 The budget

- **Scan duty cycle**, steady state with the radio present: **0 %**. A
  connected link never scans.
- **Scan duty cycle**, radio absent for hours: 30 s per 15 min once the
  ladder reaches its cap = **3.3 %**, down from today's **100 %**
  (2.2.6). This is the change with the largest expected battery effect
  in the whole spec.
- **Wakes per hour**, steady state: one per inbound packet. A quiet
  Firefly channel with four crew members and default Meshtastic
  telemetry/nodeinfo cadence is a handful per hour; a busy one during
  a set could be dozens per minute. **The app does not control this** —
  the radio decides what to forward — which is why §3.12 makes each wake
  as cheap as possible rather than trying to have fewer.
- **Target**: Firefly attributable to ≤ **5 %/day** of an iPhone battery
  in the steady connected state, measured per P8. If measurement says
  otherwise, this number changes and the spec says so — it is not a
  claim, it is a threshold to test against.

### 4.3 Things deliberately not done for battery

- No periodic BLE heartbeat (§3.8) — a scheduled background write is a
  cost with no benefit while suspended, and §1.7 says the timer would
  not fire anyway.
- No `allowDuplicates` scanning, ever — ignored in the background (§1.5)
  and expensive in the foreground.
- No RSSI polling in the background. (Meshtastic-Apple cancels theirs on
  background; we simply do not have one.)
- No `BGTaskScheduler`. There is no deferrable work here; everything is
  either event-driven or it does not need to happen.

## 5. Acceptance criteria

Marked by what can actually verify them. **[unit]** = `swift test`, no
radio, no app bundle. **[loopback]** = `swift test` driving
`LoopbackTransport`'s `simulateDisconnect()`/`simulateReconnect()`.
**[app-host]** = `xcodebuild test -only-testing:FireflyAppTests`, which
runs inside `Firefly.app` and may therefore construct a real
`CBCentralManager`. **[iPhone]** = §6 only; no automated substitute
exists and none will be faked.

1. **A03_AC1** — `BLETransport.prepareForRestoration()` constructs the
   central manager exactly once, is safe to call repeatedly, and never
   issues a `connect()` or a scan. **[app-host]** (a second call must
   not produce a second manager)
2. **A03_AC2** — the iOS central-manager options contain a **fixed**
   restore identifier and `CBCentralManagerOptionShowPowerAlertKey`
   `false`; the macOS options contain neither. **[unit]** (the options
   dictionary is already a pure `static var`, `BLETransport.swift:831`)
3. **A03_AC3** — a transport `.ready` that arrives with **no**
   `connect()` continuation outstanding starts a handshake and drives
   the client to `.ready`, with the nodeDB rebuilt exactly once.
   **[loopback]** — this is the restoration path's client half (2.2.2)
   and it is the single most important automated test in this spec.
4. **A03_AC4** — `BLETransport.powerStateAction(for:shouldAutoReconnect:
   hasPreferred:)` returns, for every `CBManagerState`, exactly the row
   in §3.5's table; `.poweredOff` never clears `shouldAutoReconnect`.
   **[unit]**
5. **A03_AC5** — `BLETransport.reconnectLadderDelay(forAttempt:)` is
   monotonically non-decreasing, reaches the 15-minute cap, stays there
   for every later attempt, and applies jitter within ±20 %. **[unit]**
6. **A03_AC6** — the ladder's "should I scan now" decision is a pure
   function of `(disconnectedAt, now, attempt, shouldAutoReconnect,
   pendingConnectPeripheralID)` and never of a sleeping task; given a
   `now` that jumps forward by an hour (the suspended-process case), it
   fires **once**, not once per skipped rung. **[unit]**
7. **A03_AC7** — a scan window closes at or before 30 s of elapsed
   *evaluated* time even when the peripheral is never discovered.
   **[unit]** for the decision, **[app-host]** for `stopScan()` actually
   being called.
8. **A03_AC8** — `AppGraph.isForegrounded` is `false` on construction;
   an inbound FLARE arriving before any scene-phase signal posts a
   notification and does **not** activate `flareTakeover`. **[unit]**
   (extends the existing
   `testInboundFlareWhileBackgroundedNeverShowsTheTakeover…`)
9. **A03_AC9** — `SettingsStore.backgroundConnectEnabled` reads `true`
   when nothing is persisted, and round-trips an explicit `false`.
   **[unit]** (replaces
   `testBackgroundConnectDefaultsFalseAndRoundTrips`)
10. **A03_AC10** — a pure `NotificationPlan` builder produces, for each
    event in §3.11.1, the exact interruption level, thread identifier,
    category identifier, derived request identifier, deep link and
    body string in §3.11.1–§3.11.3. No `UNUserNotificationCenter` is
    touched. **[unit]** — this is the seam that makes notification
    behaviour testable at all, and it is why S2 introduces it.
11. **A03_AC11** — the same packet delivered twice produces **one**
    notification request with the same identifier, not two. **[unit]**
12. **A03_AC12** — inside quiet hours, a FLARE is still
    `.timeSensitive` with sound, a DM is `.active`, and a crew message
    is `.passive` with no sound; with an untrusted wall clock (S18)
    quiet hours evaluates as **off**. **[unit]**
13. **A03_AC13** — `UNNotificationSending.post` never calls
    `requestAuthorization`; a spy authorization provider records zero
    requests across any number of posts, in any authorization state.
    **[unit]**
14. **A03_AC14** — `BackgroundConnectionStatus.line(for:)` returns
    exactly §3.10's table, and the substring "connected" (case
    insensitive) never appears for any input whose link state is not
    `.ready`. **[unit]** — a mechanical honesty check, the same shape
    `SignalTierTests` uses to forbid numbers in the signal view.
15. **A03_AC15** — a contiguous non-`.ready` span longer than 5 minutes
    inserts exactly one gap row into history, with both real timestamps,
    and none is inserted for a shorter span. **[unit]**
16. **A03_AC16** — backgrounding with `backgroundConnectEnabled` **on**
    stops Radar's and Map's recompute pumps and drops the graph tick to
    30 s; foregrounding restores exactly the pumps that were running and
    no others. **[unit]** — rewrites
    `AppGraphViewModelLifecycleTests
    .testBackgroundingWithBackgroundConnectOnLeavesRadarRunning`, whose
    assertion this deliberately reverses.
17. **A03_AC17** — every notification-producing inbound path takes a
    background task assertion before its first `await` and balances it on
    every exit path, including throws. **[unit]** via
    `BackgroundTaskAsserting` spy (begin/end counts equal, never
    negative).
18. **A03_AC18** — Diagnostics reports whether the time-sensitive
    interruption level is actually available to this build, and never
    claims a level it did not get. **[app-host]**
19. **A03_AC19** — the iPhone protocol in §6 is executed end to end on
    Jake's phone against a Heltec, with the screenshots it names
    attached to the PR, and every P-step either passes or is recorded as
    a known limitation with its measured behaviour. **[iPhone]**
20. **A03_AC20** — measured background battery attributable to Firefly
    over a ≥ 12-hour connected window is recorded (P8). No target is
    "passed" by assertion; the number is written down whatever it is.
    **[iPhone]**

## 6. The iPhone test protocol

Everything below needs: an iPhone on iOS 26 with a TestFlight build
(`app/tools/testflight.sh`, `docs/app/testflight.md`), one Heltec V3 on
the Firefly channel, and a second radio (the other Heltec, or a puck) to
send from. The **Diagnostics** screen is the only source of truth —
never a guess about what "should" be happening. Record the build number
and iOS version at the top of the results.

This extends, and does not replace, `app/README.md`'s "Manual test
procedure — background BLE (M2)"; P1/P2 below are that procedure with a
longer clock.

**Before starting:** Settings ▸ CONNECTIVITY ▸ "Stay connected in
background" must read **On** (§3.3 makes that the default — confirm the
default is what you see on a fresh install, that is itself the test).
Grant notifications when the Connect screen asks (§3.11.5).

| # | Test | Steps | Expected | Screenshot |
|---|---|---|---|---|
| **P1** | Overnight pocket | Connect. Confirm Diagnostics `CONNECTED`, note **Link uptime** and **Last heard**. Lock the phone, leave it ≥ 8 h overnight with the radio powered. Do not open the app. | Morning: reopen straight to Diagnostics. `CONNECTED`; **Last heard** under a couple of minutes. Uptime at or above elapsed time means the link genuinely held; a small uptime with a recent Last heard means it dropped and recovered — **both are passes**, and they are different results, so record which. | Diagnostics before and after |
| **P2** | Radio power cycle, backgrounded | Connect, background the app, power the Heltec off for ~10 s, back on. Wait 2 min. Foreground. | `RECONNECTING (attempt N)` appears at some point and settles to `CONNECTED` with **no** CONNECT tap. Uptime small, Last heard recent. | Diagnostics showing RECONNECTING, then CONNECTED |
| **P3** | Relaunch after jettison | Connect. Background. Open several heavy apps (camera, maps, a game) to push Firefly out of memory — confirm via Xcode ▸ Devices ▸ Console or simply by the app cold-launching later. Send a **DM** from the second radio. | A notification arrives **without the app being opened first**. Opening it lands on the thread. This is the state-restoration test (§3.1) and the one PR #272 could never verify. If no notification arrives, capture the device console filtered on `BLETransport` — that log is the evidence. | Lock-screen notification; Diagnostics after opening |
| **P4** | Bluetooth off/on | Connected, app backgrounded. Toggle Bluetooth **off in Settings**, wait 60 s, toggle on. Wait 2 min. Foreground. | Status line read **Paused — "Bluetooth is off…"** while off; `CONNECTED` on its own after. §1.2: this case does **not** relaunch the app, so it passes only while the process is still alive — if the app had been jettisoned, record that as the expected limitation, not a bug. | Status line in both states |
| **P5** | Control Centre toggle, and force-quit | (a) Same as P4 but via Control Centre. (b) Force-quit Firefly from the app switcher, then send a FLARE from the second radio. | (a) Recovers. (b) **Expected to fail on iOS 26** per TN3115 note 5 (§1.2) — no notification. Record exactly what happens; this is the measurement that decides whether AccessorySetupKit is worth adopting (§3.13). | Whatever happens, including nothing |
| **P6** | Phone reboot | Connected, app backgrounded. Reboot the phone. **Do not open Firefly.** Unlock once. Wait 5 min, then send a DM. | Per §1.2, relaunch is expected after first unlock. Notification arrives with no app launch. If not, record it — this is the second load-bearing **[unverified]** in the spec. | Lock-screen notification |
| **P7** | Radio dead for hours | Connect. Power the Heltec off and leave it off for ≥ 4 h with the app backgrounded. Then power it on. | Reconnects on its own within ~15 min of the radio returning (the ladder's cap, §3.6). Battery drain over the dead window is the number that matters — take a Settings ▸ Battery reading before and after. | Settings ▸ Battery, Firefly row, before/after |
| **P8** | Battery | A ≥ 12 h connected window with normal use. Settings ▸ Battery ▸ Firefly ▸ **Show Activity by App**, and note background vs screen-on time. Repeat once with "Stay connected in background" **off** as a control. | Numbers recorded, both runs. No pass/fail — this establishes what the feature costs (§4.2). | Settings ▸ Battery detail, both runs |
| **P9** | Notification behaviour | With the phone **locked** and Sleep Focus **on**: send (a) a FLARE, (b) a DM, (c) three crew messages in a row. | (a) breaks through Focus, lights the screen, plays a sound. (b) alerts normally. (c) arrive as **one group**, and during quiet hours make no sound. Tapping (a) opens Find ▸ Radar with that person selected; tapping (b) opens their thread. | Lock screen with all three; the grouped stack expanded |
| **P10** | Honest status | While the radio is off, read the status line on Connect and in Settings. Then pull the radio's antenna / walk 300 m away. | Never the word "connected" while the link is down; **Last heard** ages honestly; no fabricated uptime. | Status line in each state |

Anything P-step that fails gets its device console log attached. A
failing step is a finding, not a reason to soften the spec.

## 7. Slices

One spec slice per PR, per `AGENTS.md`. Tier 3 review throughout —
this is protocol/lifecycle and trust-surface work.

### S1 — restoration that can actually fire, and a reconnect that survives suspension

*Closes 2.2.1–2.2.7, 2.3.10, 2.4.18, 2.4.19.*

- `BLETransport.prepareForRestoration()`; `AppDelegate` via
  `UIApplicationDelegateAdaptor` calling it plus `AppGraph.start()`;
  `FireflyApp.init()` as the second path (§3.1).
- `MeshtasticClient.beginListening()`; the `.ready`-with-no-continuation
  handshake path (§3.1).
- `CBCentralManagerOptionShowPowerAlertKey: false` (§3.5).
- `isForegrounded` defaults `false`, seeded from
  `applicationState`/initial `scenePhase` (§3.2).
- `backgroundConnectEnabled` defaults `true` (§3.3).
- `CBConnectPeripheralOptionEnableAutoReconnect` + the iOS 17 disconnect
  delegate (§3.4).
- Real `centralManagerDidUpdateState` handling, extracted as
  `powerStateAction(...)` (§3.5).
- The bounded, clock-driven reconnect ladder (§3.6), replacing
  `armReconnectFallback`'s sleeping task.
- Background pump shutdown and the 30 s tick (§3.12), including the
  rewritten `#298` test.

Tests: A03_AC1–A03_AC9, A03_AC16. Gate: `swift build`, `swift test`,
`xcodebuild` both platforms, `FireflyAppTests`, hardware tests skipping
clean — plus `BLEReconnectHardwareTests` against **Meshtastic_06b0
only**, per PR #279's board policy.

### S2 — notifications worth waking up for

*Closes 2.3.11–2.3.17, and §3.8–§3.11.*

- A pure `NotificationPlan` builder (event → level, thread, category,
  identifier, title, body, deep link) and `NotificationSending` widened
  to take a plan (§3.11, A03_AC10).
- A real iOS entitlements file with
  `com.apple.developer.usernotifications.time-sensitive`, and the App ID
  capability enabled in the developer portal — **this is a Jake action,
  not an agent action**, and S2 is blocked on it for the
  `.timeSensitive` rows only; everything else ships without it.
- RALLY notifications; DM vs crew distinction; badge; dedupe by derived
  identifier; withdrawal on thread open (§3.11.3).
- Categories + the FLARE "Find them" action, registered at launch
  including background relaunches (§1.10).
- `UNUserNotificationCenterDelegate` on the `AppDelegate`: `willPresent`
  returning `[.banner, .list, .sound]` explicitly (§1.10 — the
  swallowing hazard), and `didReceive` routing the deep link.
- Permission asked in context, on first foreground `.ready`, never from
  the posting path (§3.11.5).
- Quiet hours setting, with the FLARE exemption mirroring `ff_sound`
  (§3.11.4).
- `lastInboundAt`, the foreground liveness probe (§3.8), the
  `BackgroundConnectionStatus` line (§3.10), and the gap marker (§3.9).
- `BackgroundTaskAsserting` (§3.14).

Tests: A03_AC10–A03_AC15, A03_AC17.

### S3 — the phone test, and the counters that make it readable

*Closes A03_AC18–A03_AC20.*

- Diagnostics counters, all of them observations rather than estimates:
  background wakes, restore events (and the `CBPeripheralState` each
  restored into), reconnect attempts, scan windows opened/closed and
  total scan seconds, notifications posted and suppressed (with the
  reason), `lastDisconnectAt` (from the iOS 17 `timestamp:`),
  `lastInboundAt`, and whether `.timeSensitive` is actually available to
  this build.
- A TestFlight build and the §6 protocol, run end to end, with the
  screenshots attached and every result recorded — including the ones
  that fail.
- The AccessorySetupKit decision (§3.13), written up with P5's measured
  outcome as its evidence.

## 8. What Meshtastic-Apple does, and what we are doing differently

Read from `~/Developer/Meshtastic-Apple` @ `14a47966` (2026-09-08).
`Meshtastic/Helpers/BLEManager.swift` no longer exists; BLE lives in
`Meshtastic/Accessory/Transports/Bluetooth Low Energy/BLETransport.swift`
and `BLEConnection.swift`, with the connect state machine in
`Accessory Manager/AccessoryManager+Connect.swift`.

**They do**, and we should copy or already have:

- A fixed restore identifier (`kCentralRestoreID = "com.meshtastic.central"`,
  their `BLETransport.swift:17`) and a full `willRestoreState` that
  branches on `CBPeripheralState` — the same shape Firefly already has.
  They go further in one way worth taking: an adopted `.connected`
  restore skips `want_config` **and** the database dump entirely (their
  `:600`). Firefly cannot (no persisted nodeDB, A01), and §3.1 says so
  rather than pretending.
- `CBCentralManagerOptionShowPowerAlertKey: false`, for a measured UX bug
  (their issue #2139). Adopted, §3.5.
- A `restoreInProgress` flag suppressing discovery while a restore is
  adopted. Adopted, §3.1.
- Not creating the central at all while authorization is
  `.notDetermined`, so the system prompt does not front-run onboarding.
  Worth copying; noted for S1.
- Per-step handshake timeouts with a **120 s watchdog on the node-DB
  dump specifically**, because a radio that completes config and never
  sends the completion nonce would otherwise wedge forever (their
  `Connect.swift:213`). Firefly already has a 120 s `nodeDBPhaseTimeout`
  (`MeshtasticClient.swift:318`) — same number, arrived at independently,
  which is mildly reassuring.
- A guard against re-requesting the node dump mid-stream (their
  `Connect.swift:196`) — two interleaved dumps is a real bug and Firefly
  should check it has the same protection.
- Withdrawing **delivered** as well as pending notifications when a
  message is read. Adopted, §3.11.3.
- Cancelling RSSI polling on background. We have no RSSI poll; nothing
  to do, but worth not adding one.

**They do not**, and this is where Firefly should diverge:

- **No `CBConnectPeripheralOptionEnableAutoReconnect`, and no iOS 17
  disconnect delegate.** Their reconnect is *discovery-driven*: a
  re-advertising radio must be re-discovered by a scan before
  `connectToPreferredDevice` fires (`AccessoryManager+Discovery.swift:67`),
  and they scan with `allowDuplicates: true`, which iOS ignores in the
  background (§1.5). That is plausibly the mechanism behind their open
  issues #722 ("Bluetooth no longer reconnects automatically") and #1171.
  Firefly's pending-connect-first design is the better one and §3.4
  strengthens it further.
- **No backoff.** Flat `maxRetries = 2`, `retryDelay = .seconds(2)`
  (`Connect.swift:13`). Firefly's bounded exponential handshake retry
  (`MeshtasticClient.swift:294`) is already better, and §3.6 adds the
  ladder they lack.
- **`interruptionLevel = .timeSensitive` on everything** — a new-node
  discovery interrupts a Focus exactly like a direct message
  (`LocalNotificationManager.swift:66`). This is the thing that trains
  users to revoke the permission. §3.11.1 differentiates by event
  instead.
- **No `threadIdentifier` on message notifications**, so a busy channel
  is a wall of banners. §3.11.3 groups.
- **`.critical` is shipped as dead code** — the entitlement is present,
  `Notification.critical` exists, and no call site ever passes `true`.
  §3.11.1 declines `.critical` outright rather than ship the middle
  state.
- **No chunking**; an oversized write is logged with "expect an ATT
  failure" (`BLEConnection.swift:593`). Firefly already refuses
  oversized payloads upstream (PR #294 finding 2), which is the better
  end of the same problem.
- **Nothing documented about background behaviour** anywhere in their
  README or docs. This file is the difference.

Their field note is worth recording verbatim because it is the kind of
thing only hardware teaches: on a Heltec V4, writes of 8–33 B succeed
while a 104 B `set_owner` is rejected at a negotiated ATT MTU of 255 —
buffer exhaustion, not a size limit. Firefly's
`insufficientResources` retry (`BLETransport.swift:636`) already handles
exactly this, borrowed from them, and it is correct.

## 9. Questions for the owner

1. **RALLY urgency.** §3.11.1 puts RALLY at `.active`, not
   `.timeSensitive`. "Meet here" is directional but not an emergency —
   is that right, or should a RALLY from crew break Focus too?
2. **Quiet-hours default.** Off by default, with 1 am–9 am suggested
   when enabled. Should it instead default **on** for the festival
   build?
3. **AccessorySetupKit.** §3.13 defers it pending P5's measurement. If
   P5 shows force-quit really is unrecoverable on iOS 26, is a Connect
   screen rewrite acceptable before Sep 18, or does the sentence in
   §3.13(2) have to carry it this year?
4. **The `beginListening()` change** (§3.1) touches the M1 connect path.
   Acceptable risk a week out, or should S1 ship the restoration half
   behind a launch flag first?
