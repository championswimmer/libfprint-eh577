# Small-sensor / partial-fingerprint matcher research: why minutiae are scarce, and how systems compensate

Date: 2026-06-12

## Question

Given that the EH577 raw press image is only about `103 x 52` pixels and we are often harvesting only **3–5 minutiae**, how are small fingerprint sensors usually used in practice?

Specifically:

- how do systems compensate for very small sensed area?
- how do they get more usable minutiae / matching signal?
- does matching usually compare against multiple enrolled partials, fused templates, or a single gallery image?

## Short answer

### 1. 3–5 minutiae is **very little**, but not surprising for such a tiny sensing area

A lot of the partial-fingerprint literature assumes mobile/embedded sensors that still capture substantially larger partials than EH577.

One representative paper notes that partial mobile prints around **`0.24" x 0.39"`** often contain **no more than ~20 minutiae**.

If EH577 is roughly a `103 x 52` raw image at around **500 dpi**, then the sensed area is only about:

- `103 / 500 ~= 0.206"`
- `52 / 500 ~= 0.104"`

So EH577 is roughly **`0.206" x 0.104"`**, i.e. **much smaller area** than the `0.24" x 0.39"` example from the literature.

That makes a harvest of only **3–5 minutiae** entirely plausible.

### 2. Small sensors usually compensate by using **multiple enrollment impressions**

The most common strategies are:

- store **multiple partial templates** for the same finger,
- or merge multiple impressions into a **super-template** / **mosaic**,
- then match a new probe against that richer enrolled representation.

### 3. Matching often succeeds if the probe matches **any one** of several enrolled partials

This is explicitly described in partial-print literature and is also exactly how libfprint's generic imaging path works:

- each successful enroll stage contributes another NBIS minutiae set to the enrolled print,
- verification tests the new scan against the stored enrolled prints,
- and returns success if **any stored print** exceeds threshold.

So, yes: in practice, **multiple gallery partials per finger are common**.

### 4. To improve small-sensor performance, systems usually add **coverage**, **quality filtering**, and **richer features** — not just "more minutiae from one tiny frame"

The broad direction is:

- collect more impressions,
- cover more finger area,
- reject low-quality / edge-heavy captures,
- remove false minutiae,
- and use extra local texture / ridge descriptors when minutiae count is too low.

## Internet / literature findings

### A. Multiple enrollment impressions are standard for small/partial sensors

The University of Nevada / DigitalPersona paper on **minutiae-based template synthesis** states that one effective way to deal with small sensing area, missing minutiae, and spurious minutiae is to capture **multiple enrollment impressions** and combine them.

It describes two standard strategies:

1. **score-level fusion**: match the query against each enrolled impression and fuse scores / decisions,
2. **template synthesis**: merge multiple enrollment feature sets into a **"super-template"**.

It also states that:

- multiple impressions increase **coverage area**,
- can restore **missing minutiae**,
- can suppress **spurious minutiae**,
- and address the **small overlap** problem.

Source:

- [`/home/championswimmer/Downloads/minutiae-based-template-synthesis-and-matching-for-fingerprint-authentication.md`](/home/championswimmer/Downloads/minutiae-based-template-synthesis-and-matching-for-fingerprint-authentication.md)

### B. Some mobile partial-print systems really do match a query against a **set of enrolled partial templates**

The partial high-resolution matching paper summarized by search results says mobile authentication with small sensors often works by:

- enrolling **several partial templates** from the same finger,
- then matching the query partial against that **set**.

It specifically describes:

- query-vs-template matching **one by one**,
- selecting the best candidate,
- then doing richer minutiae-based scoring / fusion.

This is a direct answer to the "multiple gallery images?" question: **yes, often multiple partial templates are used**.

Source:

- https://www.sciencedirect.com/science/article/abs/pii/S0167865517303227

### C. Small sensors have a known **MasterPrint / false-match vulnerability**

The DeepMasterPrint and MasterPrint literature repeatedly emphasizes that:

- small sensors capture only **partial** fingerprints,
- partials are **less distinctive** than full prints,
- and systems often store **multiple partial readings per finger**.

That combination creates more opportunities for impostor matches, because a probe only needs to match **one** enrolled partial template.

The DeepMasterPrint paper explicitly says consumer mobile devices commonly:

- use **small sensors**,
- store **multiple partial readings** for one finger,
- and verify by comparing against those stored partials.

This is very relevant to EH577 false matches: if the probe itself is tiny and low-information, and enrollment stores multiple partials, then a permissive threshold can create accidental overlaps.

Source:

- [`/home/championswimmer/Downloads/bontragerrossdeepmasterprint-btas2018.md`](/home/championswimmer/Downloads/bontragerrossdeepmasterprint-btas2018.md)

### D. Less area mainly hurts **false non-match** rate, and can be mitigated by more area or more fingers

The NIST mobile-ID study found that reducing spatial area (e.g. FAP10 vs larger captures) causes a significant performance penalty, especially in **missed matches**.

NIST's mitigation suggestions were essentially:

- capture **more area**, or
- use **more fingers**.

This aligns with the broader literature: area loss is fundamentally expensive, and the main way out is **more biometric evidence**, not magical matching.

Source:

- https://www.nist.gov/publications/examination-impact-fingerprint-spatial-area-loss-matcher-performance-various-mobile

### E. When minutiae are too few, papers add **richer descriptors** around the minutiae

Several partial-print papers note that plain minutiae geometry becomes weak when the overlap is small and the count is low.

Typical compensations include:

- ridge-orientation descriptors around minutiae,
- local ridge/valley structure,
- texture descriptors like SIFT-style or learned descriptors,
- pores when the sensor resolution is high enough,
- global coarse descriptors fused with minutiae-level matching.

So the industry/research answer to "how to get more signal?" is often **not** "extract more minutiae from the same tiny frame", but rather:

- attach more **context** to each minutia,
- or use additional non-minutiae features.

Representative sources:

- https://cedar.buffalo.edu/~govind/partial.pdf
- https://www.mdpi.com/1424-8220/13/3/3142/pdf
- https://www.jstage.jst.go.jp/article/transinf/E100.D/3/E100.D_2016EDP7256/_pdf
- https://www.sciencedirect.com/science/article/abs/pii/S0167865517303227

## What libfprint itself does today

### Generic imaging path: enrollment stores multiple NBIS minutiae sets

The generic imaging-device path in libfprint:

1. extracts minutiae from each successful image,
2. wraps them in an `FPI_PRINT_NBIS` print,
3. and during enroll appends each stage's print into the enrolled template.

Relevant local code:

- [`refs/libfprint/libfprint/fpi-print.c`](../../../refs/libfprint/libfprint/fpi-print.c)
  - `fpi_print_add_print()` appends another single NBIS print into the enrolled collection.
- [`refs/libfprint/libfprint/fpi-image-device.c`](../../../refs/libfprint/libfprint/fpi-image-device.c)
  - during enroll, successful stage prints are added into `enroll_print` via `fpi_print_add_print()`.

### Generic imaging verify: probe is matched against **all stored enrolled prints**

`fpi_print_bz3_match()` is documented and implemented to:

- treat the new scan as exactly **one** print,
- and compare it against the **prints contained in the template**.

It loops over `template->prints` and returns success if **any** score reaches threshold.

Relevant local code:

- [`refs/libfprint/libfprint/fpi-print.c`](../../../refs/libfprint/libfprint/fpi-print.c)
  - `fpi_print_bz3_match()`
- [`refs/libfprint/libfprint/fpi-image-device.c`](../../../refs/libfprint/libfprint/fpi-image-device.c)
  - verify path calls `fpi_print_bz3_match(template, print, bz3_threshold, ...)`

So for generic imaging drivers, libfprint is effectively doing:

- **multi-impression enrollment**
- **one-probe vs many-enrolled-partials matching**

rather than synthesizing a single fused super-template.

## Important implication for EH577

### EH577 is likely in the danger zone where "multi-partial enrollment + weak single probes + low threshold" can produce false matches

Repo-local facts:

- EH577 raw snapshot is only about `103 x 52` pixels.
- The current driver threshold is:
  - [`EGIS0577_BZ3_THRESHOLD 9`](../../../refs/libfprint/libfprint/drivers/egis0577.h)
- libfprint's generic imaging default documents `bz3_threshold` as:
  - **default 40** in [`fpi-image-device.h`](../../../refs/libfprint/libfprint/fpi-image-device.h)

That does **not** mean 40 is universally correct, but it does show how aggressive the EH577 reduction is.

There is even a blunt historical comment in the older EH570 code noting that lowering the threshold was a security tradeoff:

- [`refs/libfprint/libfprint/drivers/egis0570.h`](../../../refs/libfprint/libfprint/drivers/egis0570.h)

So if EH577 probe images truly only contain ~3–5 reliable minutiae, then a low threshold plus OR-ing across multiple enrolled partials is a very plausible recipe for **false accepts**.

## Practical answers to your specific questions

### How are such small sensors usually used?

Usually by combining **multiple touches / multiple partial impressions** for enrollment, and then either:

- storing multiple partial templates, or
- synthesizing a larger/fused template.

### How do they get more minutiae?

Mostly by getting **more coverage across multiple impressions**, not by expecting one tiny capture to suddenly yield a full-size minutiae set.

Also by:

- improving image quality,
- removing false minutiae,
- guiding the finger toward central / discriminative areas,
- and attaching extra ridge/texture descriptors to each minutia.

### How does matching work?

Common patterns are:

1. **probe vs each enrolled partial template**, take best / any-above-threshold,
2. **probe vs fused super-template**,
3. or hybrid methods using both minutiae and texture/global descriptors.

### Does it match against multiple gallery images?

**Yes, very often.**

And for libfprint's generic imaging path, the answer is concretely **yes**: the enrolled template can contain multiple NBIS stage prints, and verification succeeds if the probe matches **any** stored stage print.

## Practical implication for EH577 bringup

The research strongly suggests that for EH577, the path to correctness is probably **not**:

- keep the same tiny probe evidence,
- keep a very low threshold,
- and hope Bozorth alone behaves.

More promising directions are:

1. **Improve per-capture quality**
   - better finger-present gating
   - better capture timing / stability
   - background / hot-pixel suppression
   - reject weak/edge-heavy samples

2. **Improve enrollment representation**
   - keep multiple high-quality partials, but maybe quality-rank them
   - consider whether stage selection / dedup / pruning would help
   - consider super-template / merged-template experiments offline

3. **Add richer matching evidence**
   - local ridge-orientation / texture descriptors around minutiae
   - not just raw minutiae count + Bozorth score

4. **Be very careful with threshold reduction**
   - low thresholds can rescue FNMR but can badly hurt false-match behavior on tiny partials

## Bottom line

For a sensor this small, the usual industry/research answer is:

- **multiple impressions**,
- **multiple enrolled partial templates or a fused super-template**,
- **quality filtering**,
- and often **more than plain minutiae geometry**.

Given EH577's tiny area, getting only **3–5 minutiae** from a single probe is not shocking.

The bigger issue is that such a weak single probe makes generic **probe-vs-any-enrolled-partial** matching very vulnerable unless:

- the captures are very clean,
- the enrolled partials are curated well,
- and the matcher/threshold are conservative enough.
