# Press Sensor Capture Timing Strategies

An analysis of other `FP_SCAN_TYPE_PRESS` drivers in `libfprint` reveals several distinct strategies for handling finger placement and image capture timing. The choice of strategy heavily depends on whether the sensor's firmware abstracts away the finger placement timing or if the driver must manage a raw video stream.

## 1. Hardware/Firmware Managed Timing

The majority of modern press sensors handle finger settlement in hardware, allowing the driver to avoid explicit software delays.

### AuthenTec AES3000 (`aes3k.c`)
- **Strategy:** Blocking USB Transfer
- **Mechanism:** The driver submits a bulk USB transfer request (`do_capture`) to read the image. The sensor hardware itself blocks the transfer and does not return any data until a finger is pressed and the image is captured.
- **Timing:** No explicit software delay. The driver trusts that when the bulk transfer completes successfully, the hardware has already waited for the finger to settle and captured a usable image.

### NB1010 (`nb1010.c`)
- **Strategy:** Polling hardware status registers
- **Mechanism:** The driver continuously polls a hardware register (`M_CHECK_PRINT`) to ask if a finger is present. Once the hardware reports a finger is present, the driver *immediately* sends a command to capture the image.
- **Timing:** The hardware firmware determines when the finger is fully present. The driver assumes the first frame available after the hardware's "finger present" signal is correctly settled and usable. No explicit software delay is used.

### VFS7552 (`vfs7552.c`)
- **Strategy:** Hardware interrupts combined with software heuristics
- **Mechanism:** The driver waits for the sensor to report a `DATA_READY` state. Once data is ready, it captures the image chunk. To ensure the image is a fully settled finger, it computes the variance (`fpi_std_sq_dev`) of the captured image. If the variance is above `CAPTURE_VARIANCE_THRESHOLD` and below `NOISE_VARIANCE_THRESHOLD`, the capture is accepted.
- **Timing:** No hardcoded time delays. Instead, it continuously captures frames as fast as the hardware allows until a frame satisfies the variance constraints. An approaching finger has low variance, while a settled finger has high variance.

## 2. Software Managed Streaming

When the sensor is a "dumb" imager that just streams raw frames continuously without any internal finger-detect logic, the driver is forced to manage the timing.

### DigitalPersona URU4000 (`uru4000.c`)
- **Strategy:** Continuous streaming
- **Mechanism:** The driver loops in an `IMAGING_CAPTURE` state, continuously fetching frames from the device as fast as possible. Each frame is passed directly to `libfprint` via `fpi_image_device_image_captured`.
- **Timing:** No explicit delay is introduced by the driver. It relies on `libfprint`'s internal enrollment/identification state machine to decide whether the streamed frames are good enough (e.g., using minutiae scores or internal variance checks).

## Conclusion for EGIS0577

The Egis 0577 driver currently treats the sensor as a "dumb" streaming device (capturing raw 144x150 frames in a continuous loop). Because we are manually detecting finger presence via a pixel threshold (`nonzero > 1000`), we do not have the luxury of hardware-managed settlement. 

If we capture the very first frame that crosses the 1000-pixel threshold, the finger is often still in motion or lightly touching, resulting in partial or smudged prints. Therefore, our newly introduced explicit `200ms` software delay after the initial threshold trigger is a completely valid and necessary strategy for a streaming sensor without hardware-assisted capture timing. It mimics the "settling" delay that other chips handle in firmware.
