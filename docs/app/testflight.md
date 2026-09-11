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
- Bundle ID: **`com.jakeholland.firefly`**
  — if it is not offered in the picker, register it first under
  **Certificates, Identifiers & Profiles -> Identifiers -> "+"**
  (App IDs, explicit, `com.jakeholland.firefly`), then come back.
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
