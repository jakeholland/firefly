# firebase/

`firestore.rules` — the security rules for project `firefly-36297`
(A04, `docs/specs/A04-telemetry.md`). This is the SOURCE of the rules,
committed and reviewable here; it is not deployed automatically by CI.

Deploy it by hand (owner-only — this needs the Firebase CLI and a login
against `firefly-36297`):

```
npm install -g firebase-tools   # once
firebase login                  # once
firebase deploy --only firestore:rules --project firefly-36297
```

No `firebase.json`/`.firebaserc` ship in this repo yet — the console
project already exists (see `app/README.md`, "Reading the data"), and
either a one-time `firebase init` against it or a small follow-up PR
adding those two files is enough to make the command above work as
written. Until then, the rules can be pasted into the Firestore console
(Firestore Database → Rules) directly from this file.

See `app/README.md` for what the app actually writes, how "Share
diagnostics" / "Export diagnostics" work, and how to read the data back
(console vs. local export).
