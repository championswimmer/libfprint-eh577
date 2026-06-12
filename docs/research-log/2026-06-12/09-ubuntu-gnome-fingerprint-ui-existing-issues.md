# Ubuntu / GNOME fingerprint UI issue scan: no exact `11/15` bug found on Questing, but related UI issues exist

Date: 2026-06-12

## Question

Is the lack of explicit numeric enrollment progress in GNOME Settings on **this OS** an existing known issue?

## Local OS / package context

On this machine:

- OS: **Ubuntu 25.10 (Questing Quokka)**
- Kernel: `6.17.0-35-generic`
- GNOME Settings / `gnome-control-center`: **49.0** (`1:49.0-1ubuntu2.1`)
- `fprintd`: `1.94.5-2`
- `libfprint`: `1:1.94.9+tod1-1ubuntu0.2`

Source: local `uname`, `/etc/os-release`, `gnome-control-center --version`, and `dpkg -l`.

## Short answer

**I did not find an existing Ubuntu 25.10 / GNOME 49 bug specifically about "Settings should show `11/15` during fingerprint enrollment".**

What I *did* find is:

1. **GNOME definitely tracks enrollment progress internally**.
2. There is **historical prior art** for bugs where the UI showed the *wrong amount* of stage feedback.
3. There are **other current Ubuntu fingerprint UI bugs** in `gnome-control-center`, just not this exact one.

So the absence of `11/15` in your current Ubuntu UI looks more like a **design / presentation choice** than a currently-tracked backend limitation.

## Findings

### 1. GNOME Settings already has an enrollment-progress view and internal progress tracking

The GNOME fingerprint dialog redesign explicitly introduced:

- an **"enrollment progress view"**,
- local counters like `enroll_stages_passed`,
- and a computed fraction `enroll_progress`.

The implementation increments the local counter on `enroll-stage-passed` and divides by `num-enroll-stages`.

This means GNOME Settings knows progress numerically, even if it does not render it as `11/15` text.

Sources:

- https://lists.gnome.org/archives/commits-list/2020-June/msg11360.html
- https://gitlab.gnome.org/GNOME/gnome-control-center/-/merge_requests/741

### 2. There is historical evidence that fingerprint-stage visualization in GNOME Settings has been a real UX bug area

A Red Hat bug from the older `gnome-control-center` UI reported:

> "Enrolling fingerprint requires more finger touches than shown in control-center"

Specifically:

- the UI showed **5** placements,
- but the device required **8**,
- and the fix was to show more stage icons.

This is directly relevant because it shows that:

- users *do* notice when the UI under-communicates enrollment stage count,
- and upstream/downstream have previously treated this as a UI bug worth fixing.

Sources:

- https://bugzilla.redhat.com/show_bug.cgi?id=1789474
- https://mail.gnome.org/archives/commits-list/2019-July/msg12023.html

### 3. GNOME later generalized the UI to avoid a hardcoded max-stage limit

After the 5->10 stage expansion, GNOME also landed a change described as:

- `fingerprint-dialog: Don't limit the number of maximum enroll stages`

That further confirms the UI stack has long cared about representing device-specific enrollment stage counts correctly.

Source:

- https://lists.gnome.org/archives/commits-list/2020-June/msg11380.html

### 4. On Ubuntu, I found related fingerprint UI bugs — but not this exact one

Examples found:

- **LP #2099838**: "Scan new fingerprint" button does nothing until a fingerprint is first enrolled with `fprintd-enroll` (Ubuntu 24.10, `gnome-control-center` 47.x). This is a fingerprint UI bug, but not about numeric progress.
  - https://bugs.launchpad.net/ubuntu/+source/gnome-control-center/+bug/2099838
- **LP #2076164**: wrong remaining finger list / wrong finger removed from the GUI after enrollment (Ubuntu 24.04 OEM). Fixed in later Ubuntu packages.
  - https://bugs.launchpad.net/oem-priority/+bug/2076164
- **LP #1873298**: older GNOME Settings could not enroll multiple fingers properly. Fixed years ago.
  - https://bugs.launchpad.net/bugs/1873298

So Ubuntu absolutely has had fingerprint-UI issues in `gnome-control-center`, but I did **not** find a Launchpad bug matching:

- "show explicit enrollment count"
- "show `N / total`"
- "show remaining touches/stages"

for **Ubuntu 25.10 / GNOME 49**.

### 5. GNOME release notes show ongoing fingerprint-management fixes, but nothing obviously about numeric stage text

Searches over GNOME `NEWS` turned up items like:

- `Show correctly the remaining list of fingerprints to enroll`
- `Bring back Fingerprint dialog`
- `Various improvements to fingerprint management`

These indicate active maintenance in this area, but I did not find a release-note item saying GNOME added or removed explicit numeric progress text during enrollment.

Sources:

- https://gitlab.gnome.org/GNOME/gnome-control-center/-/raw/gnome-46/NEWS
- https://gitlab.gnome.org/GNOME/gnome-control-center/-/raw/gnome-47/NEWS
- https://gitlab.gnome.org/GNOME/gnome-control-center/-/raw/main/NEWS

## Interpretation

Best current interpretation:

- **Not seeing `11/15` on Ubuntu 25.10 is probably not a known Questing-specific bug I could find.**
- It is more likely that GNOME Settings intentionally presents enrollment progress as:
  - visual stage indicators / progress animation,
  - success / retry messages,
  - and completion state,
  rather than explicit numeric text.

That said, there is good precedent for filing this as a **UI/UX issue or feature request**, because:

- GNOME already has the data,
- previous stage-visibility problems were considered valid bugs,
- and high-stage-count devices (like `15`) make numeric progress more useful than on older `5`-step devices.

## Practical recommendation

If you want this tracked upstream/downstream, the strongest bug/feature-request framing would be:

> GNOME Settings on Ubuntu 25.10 already receives enough enrollment progress information from `fprintd`, but the UI does not expose explicit numeric progress (`completed / total`). On high-stage devices (e.g. `15` stages), this makes enrollment progress much less clear than it could be.

Suggested target:

- first: **GNOME Settings / `gnome-control-center` upstream** as a UI/UX request
- optionally: **Ubuntu Launchpad** if you want downstream tracking for Ubuntu packages

## Conclusion

For **Ubuntu 25.10 / GNOME Settings 49.0**:

- **I found no exact existing public bug for "show `11/15` during fingerprint enrollment"**.
- **I did find related fingerprint UI bugs and historical evidence that enrollment-stage visibility has mattered before.**
- So this looks like a **reasonable new issue / enhancement request**, not something obviously already tracked for your current OS version.
