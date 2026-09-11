# Shipping a TestFlight build

`app/tools/testflight.sh` archives, exports and uploads a Release build
of Firefly to TestFlight. This file is the one-time App Store Connect
setup the owner does before the first real upload, plus the routine
command everyone else needs afterward. See that script's own header
comment for exactly what each step does; this is the walkthrough.

## One-time setup (owner only)

Do this once, in [App Store Connect](https://appstoreconnect.apple.com),
under team **SU4T96VBX6**.

### 1. Create the app record

**Apps -> "+" -> New App**

- Platform: **iOS**
- Name: whatever you want the TestFlight listing to show
- Primary language: your choice
- Bundle ID: **`com.jakeholland.Firefly`**
  — if it is not offered in the picker, register it first under
  **Certificates, Identifiers & Profiles -> Identifiers -> "+"**
  (App IDs, explicit, `com.jakeholland.Firefly`), then come back.
- SKU: your choice (not user-visible)

### 2. Create an App Store Connect API key

**Users and Access -> Integrations -> App Store Connect API -> "+"**

- Name: your choice (e.g. "Firefly CI")
- Access: **App Manager** — the role `testflight.sh` needs to upload
  builds and manage TestFlight testers. (Admin also works; App Manager
  is the least-privileged role that does.)

Apple shows the **.p8 private key file for download exactly once**.
The moment you create the key:

1. Download it and move it to `~/.private_keys/AuthKey_<KEY_ID>.p8`
   (create that directory if it does not exist — `mkdir -p
   ~/.private_keys`). **Never put it inside this repo.**
2. Note the **Key ID** and **Issuer ID** shown on the same page — you
   will need both every time you run the script (Apple does not show
   the issuer ID again on the key's own row afterward, only on the
   page's own header).

Then, in your shell environment (`~/.zshrc` or similar — never in a
file this repo tracks):

```sh
export ASC_KEY_ID=<key id>
export ASC_ISSUER_ID=<issuer id>
export ASC_KEY_PATH=~/.private_keys/AuthKey_<key id>.p8
```

`app/tools/testflight.sh` reads these three, and only these three, to
authenticate the upload. It never prints, logs, or copies the key
itself anywhere — it only checks that the three variables are set and
that the file at `ASC_KEY_PATH` exists, then hands that path straight
to `xcodebuild -exportArchive -authenticationKeyPath ...`.

### 3. Add Taylor as a tester

Once the first build has been uploaded and finished Apple's processing
(usually a few minutes, sometimes longer the first time — App Store
Connect emails when it's ready):

**TestFlight tab (on the app's page) -> Internal Testing** (fastest —
no Beta App Review needed, but testers must be added to the team) **or
External Testing** (works for anyone with just an email/Apple ID, but
the FIRST external build needs Apple's Beta App Review, roughly a day)

- Internal: **App Store Connect Users -> "+"** (Taylor needs a role on
  the team first, e.g. "Developer" or below, under **Users and
  Access**), then add them to the internal testing group.
- External: create a group under **External Testing -> "+"**, add
  Taylor's email as a tester, and submit the build for Beta App Review
  the first time.

Either way, Taylor gets an email invite and installs the **TestFlight**
app from the App Store to accept it and install Firefly.

### 4. Export compliance

App Store Connect normally asks, per build, whether the app uses
encryption. This is already answered in the app itself —
`Info.plist`'s `ITSAppUsesNonExemptEncryption` is `NO` (Firefly only
uses standard, exempt encryption: the OS's own HTTPS stack and
CoreBluetooth's link-layer security — nothing custom) — so App Store
Connect should not prompt for it at all once a build with that key
lands.

## Routine use (everyone, after setup)

```sh
app/tools/testflight.sh
```

This:

1. wires up `app/Config/Local.xcconfig` for team `SU4T96VBX6` if it
   does not already exist (git-ignored, one-time, per machine);
2. sets the build number to `git rev-list --count HEAD` and
   regenerates `Firefly.xcodeproj` from `project.yml`;
3. archives a Release build for iOS
   (`xcodebuild archive ... -allowProvisioningUpdates`);
4. exports and, if `ASC_KEY_ID` / `ASC_ISSUER_ID` / `ASC_KEY_PATH` are
   all set and the key file exists, uploads it to App Store Connect.

Without those three set, it still archives and exports a local `.ipa`,
then fails with the exact checklist above (steps 1-2) printed to the
terminal — that is deliberate: a `.ipa` nobody uploads is not a
finished TestFlight build.

To just prove the archive step alone still works — no export, no
upload, no App Store Connect credentials needed — use:

```sh
app/tools/testflight.sh --archive-only
```

This is what to run to sanity-check signing after any project.yml,
entitlements, or Info.plist change, before trying a real upload.

MARKETING_VERSION (currently 0.1.0) is bumped by hand in `project.yml`
when it actually changes; `CURRENT_PROJECT_VERSION` (the build number)
is never touched by hand — see `project.yml`'s own comment on that
setting.

Once uploaded, the build shows up under the app's **TestFlight** tab in
App Store Connect after Apple finishes processing it (again, usually a
few minutes). Testers already added (above) get notified automatically
for internal testing, or once you submit the build to their external
group.

## Testers

Firefly Festival Compass (`com.jakeholland.Firefly`, App Store Connect app id
`6811207165`) distributes builds through TestFlight to two different kinds of
tester, and they don't get a build the same way:

- **External testers** — anyone added to a named beta group (e.g. a `Crew`
  group of friends). The **first build offered to an external group must pass
  Apple's Beta App Review** before any tester in that group can install it —
  this typically takes about a day, sometimes longer. Every build after the
  first review approval goes out without a new review, as long as the app's
  export-compliance/encryption answers don't change. Until that first review
  clears, testers you've added will show as invited but have nothing to
  install yet.
- **Internal testers** — people added directly under Users and Access with an
  App Store Connect role (Admin, App Manager, Developer, etc.), not through a
  beta group. Internal testers get **immediate** access to any build already
  processed for TestFlight — no Beta App Review wait. The tradeoff is they
  must already be a member of the App Store Connect team, so this only works
  for people the owner is willing to add to the team itself.

**The owner decides which kind fits a given person.** Someone outside the
team (a friend testing at a festival, a reviewer) has to be an external
tester and has to wait on the first review. Someone the owner wants to give
a permanent, review-free path to every build has to be added as an internal
team member instead — `asc_testers.sh` only manages external beta-group
testers; adding an internal tester is a Users and Access action done in the
App Store Connect UI (or via the `/v1/users` API), not this tool.

### `app/tools/asc_testers.sh`

A small bash + curl + python3 tool that drives the App Store Connect API
directly (no Fastlane, no App Store Connect API client library) to manage
external beta-group testers:

```
app/tools/asc_testers.sh list-groups
app/tools/asc_testers.sh ensure-group <name>
app/tools/asc_testers.sh add-tester <email> <first> <last> <group>
app/tools/asc_testers.sh list-testers <group>
```

It authenticates with an ES256-signed JWT built locally from an App Store
Connect API key (`ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_KEY_PATH` in the
environment — see the script's header comment for the full credential-
handling contract: the private key's contents are never read into the shell,
logged, or echoed, only its path is handed to `openssl dgst -sign`, and the
signed token itself is never printed).

`ensure-group <name>` creates the group if it doesn't already exist, with
feedback enabled and the public link disabled (so testers only get in by
being added by email — nobody can self-enroll off a shared link).
`add-tester` is idempotent: if the tester already exists in App Store
Connect it's linked to the requested group rather than re-created.
