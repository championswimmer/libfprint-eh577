# 2026-06-12 — sensor size research and EH577 image-dimension assessment

Question investigated: are the current EH577 image dimensions in the driver correct, given that the physical reader looks square from the outside, and how does that compare to other touch fingerprint readers?

## Local code and artifact findings

### 1. The raw EH577 frame size in the current driver is internally consistent

The active driver defines the raw frame as `103 x 52` in [`egis0577.h`](../../../refs/libfprint/libfprint/drivers/egis0577.h):

- `EGIS0577_IMGWIDTH 103`
- `EGIS0577_IMGHEIGHT 52`
- final frame command expects `5356` bytes

That mapping is exact:

- `103 * 52 = 5356`

So the current code is not using an arbitrary width/height guess. It is treating the full `64 14 ec` payload as a single grayscale frame.

### 2. The current runtime path is a single-frame snapshot path, not the older strip-assembly path

The active implementation in [`egis0577.c`](../../../refs/libfprint/libfprint/drivers/egis0577.c):

- captures one `5356`-byte frame into `self->capture_frame`
- creates an `FpImage` with padded width `104` and height `52`
- copies each `103`-byte row into that padded image
- resizes by `EGIS0577_RESIZE = 2`
- reports final libfprint image size as:
  - `img_width = EGIS0577_PADDED_IMGWIDTH * EGIS0577_RESIZE`
  - `img_height = EGIS0577_IMGHEIGHT * EGIS0577_RESIZE`

So the current effective image geometry is:

- raw frame: `103 x 52`
- padded intermediate frame: `104 x 52`
- final reported image: `208 x 104`

This matches current saved stage images in the repo root:

- [`../../../enrolled.pgm`](../../../enrolled.pgm)
- [`../../../identify.pgm`](../../../identify.pgm)

Both were checked and are `208 x 104` PGM images.

### 3. Older EH575-style assembly produced a taller final image

The archived EH575 reference patch still shows the older strip model:

- raw width `103`
- useful strip height `24`
- multiple consecutive captures assembled into one taller image

The older assembled EH577 artifact in this repo is:

- [`../../../dumps/libfprint-guided-20260610-000413/finger-assembled-136x178.pgm`](../../../dumps/libfprint-guided-20260610-000413/finger-assembled-136x178.pgm)

So there are now two distinct image models in the project history:

1. **assembled strip image**: around `136 x 178`
2. **single-frame snapshot image**: `208 x 104` after padding + 2x resize

## Comparison with other libfprint touch / press readers

Checked local driver code under [`../../../refs/libfprint/libfprint/drivers/`](../../../refs/libfprint/libfprint/drivers/):

- [`aes4000.c`](../../../refs/libfprint/libfprint/drivers/aes4000.c): press sensor, `96 x 96`
- [`aes3500.c`](../../../refs/libfprint/libfprint/drivers/aes3500.c): press sensor, `128 x 128`
- [`vfs7552_proto.h`](../../../refs/libfprint/libfprint/drivers/vfs7552_proto.h): `112 x 112`
- [`nb1010.c`](../../../refs/libfprint/libfprint/drivers/nb1010.c): `256 x 180`
- [`upektc_img.c`](../../../refs/libfprint/libfprint/drivers/upektc_img.c): depending on sensor, `192 x 270`, `156 x 216`, `186 x 270`, `144 x 384`, `108 x 384`

Key observation: touch/press readers are **not always square**, but many common area sensors are larger than EH577's current raw `103 x 52` frame.

## Comparison with nearby Egis readers

- [`egis0570.h`](../../../refs/libfprint/libfprint/drivers/egis0570.h) uses `114 x 57`
- the archived EH575 work also uses `103 x 52` raw frames
- external EH576 community work reports a still smaller touch image around `70 x 57`

So small, non-square Egis raw frames are plausible. The square physical package does **not** prove that the active pixel matrix must also be square.

## Internet findings on common fingerprint image sizes

### Common touch / capacitive reader sizes found

From vendor docs and libfprint-visible hardware families, common touch/area image sizes include:

- `96 x 96`
- `112 x 112`
- `128 x 128`
- `160 x 160` class sensors
- larger enterprise capacitive sensors such as `256 x 360`

Examples found during web research:

- HID EikonTouch TC510 / TC710: `256 x 360` at 508 dpi
- optical desktop readers such as Futronic FS80/FS88: `320 x 480` at 500 dpi

Useful source URLs:

- https://www.hidglobal.com/products/eikontouch-510
- https://idency.com/wp-content/uploads/2023/02/HID-eikontouch-tc510-reader-datasheet.pdf
- https://www.igel.com/wp-content/uploads/2022/10/eat-hid-eikontouch-tc710-ds-en_1.pdf
- https://www.futronic-tech.com/pro-detail.php?pro_id=1543
- https://www.futronic-tech.com/pro-detail.php?pro_id=1535

### Egis external clues

Egis marketing material and teardown material suggest that:

- outer package/button shape and active sensing area are not the same thing
- square button packaging can still contain a non-square active sensing array
- small touch sensors are normal in Egis mobile-oriented products

Useful source URLs:

- https://www.egistec.com/technology/fingerprint-touch-sensor/
- https://www.egistec.com/technology/
- https://www.biometricupdate.com/201503/egistec-discusses-their-eh570-fingerprint-touch-sensor
- https://www.slideshare.net/slideshow/egistec-et300-fingerprint-sensor-2016-teardown-reverse-costing-report-published-by-yole-developpement/58606697

## Conclusion

### What looks correct

- The **transport and read mechanism** look correct.
- Treating the `64 14 ec` reply as a full raw frame is supported by the exact `5356 = 103 * 52` mapping.
- Therefore the current **raw frame size `103 x 52` is likely real**.

### What is still not proven

- The current **final biometric image geometry** is not proven to be optimal.
- The current `208 x 104` output is just:
  - `103 x 52`
  - padded to `104 x 52`
  - then doubled in each direction
- That means it is a software-resized rectangular snapshot, not independent evidence of the sensor's true useful matching area.

### Practical assessment for EH577

Best current interpretation:

1. **Raw acquisition geometry:** `103 x 52` — likely correct
2. **Best final matching geometry:** still unresolved

The physical reader looking square from the outside is not enough reason to reject `103 x 52` as a raw frame size. However, it is a reason to stay skeptical that the current single-frame `208 x 104` snapshot path is the final or best image shape for matching quality.

A plausible remaining possibility is that the raw frame size is correct, but a **multi-frame or differently processed final image** will still be needed for robust matching.
