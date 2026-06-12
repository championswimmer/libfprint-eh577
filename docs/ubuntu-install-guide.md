# How to Build and Install the EH577 Driver on Ubuntu

This guide describes how to build the custom `libfprint` driver for the **EgisTec EH577** (`1c7a:0577`) fingerprint sensor and install it system-wide on your local Ubuntu system, replacing the stock library.

> [!WARNING]
> Replacing system-wide libraries can affect authentication (like GDM login, `sudo` via PAM, etc.). Follow the instructions carefully, and ensure you keep a terminal open or remember the rollback steps in case anything goes wrong.

---

## 1. Install Build Dependencies

To build `libfprint` from source, you need to install the required build tools and development headers.

### Option A: Using `apt build-dep` (Recommended & Easiest)
If you have source repositories enabled in `/etc/apt/sources.list`, you can automatically fetch all build dependencies for `libfprint`:

```bash
sudo apt update
sudo apt build-dep libfprint-2
```

### Option B: Installing Dependencies Manually
If `build-dep` is not available, install the required packages manually:

```bash
sudo apt update
sudo apt install -y \
  meson \
  ninja-build \
  git \
  build-essential \
  libglib2.0-dev \
  libgusb-dev \
  libpixman-1-dev \
  libgirepository1.0-dev \
  libnss3-dev \
  libssl-dev \
  libgudev-1.0-dev
```

---

## 2. Exact Installation Commands (From Repo Root)

Run the following commands from the repository root directory (`/home/championswimmer/Development/Cpp/libfprint-eh577`):

### A. Configure and Build the Driver
This configures the build using the system prefix `/usr` and the correct Ubuntu library path, and compiles the source code:

```bash
# Configure the build directory for system install path
meson setup --reconfigure refs/libfprint/build refs/libfprint \
  --prefix=/usr \
  --libdir=lib/x86_64-linux-gnu \
  -Dpixman=enabled

# Compile the library
ninja -C refs/libfprint/build
```

### B. Stop Daemon and Backup System Files
Before installation, stop the system daemon to release the USB device and backup your stock files:

```bash
# Stop the active system daemon
sudo systemctl stop fprintd

# Backup the original system libfprint library
sudo cp /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0.bak

# Backup the original udev rules if they exist
sudo cp /lib/udev/rules.d/70-libfprint-2.rules /lib/udev/rules.d/70-libfprint-2.rules.bak 2>/dev/null || true
```

### C. Install and Restart Daemon
This installs the compiled library, reloads the udev rules, and restarts `fprintd`:

```bash
# Install the custom libfprint and overwrite system files
sudo ninja -C refs/libfprint/build install

# Reload and trigger udev to recognize the device with the new rules
sudo udevadm control --reload-rules
sudo udevadm trigger

# Restart the fprint daemon
sudo systemctl restart fprintd
```

---

## 3. Run Local Validation (Before System Install)

If you wish to test the driver locally **before** modifying your system files:

1. Stop the active daemon:
   ```bash
   sudo systemctl stop fprintd
   ```
2. Run the local `enroll` example using `LD_LIBRARY_PATH`:
   ```bash
   sudo env LD_LIBRARY_PATH=refs/libfprint/build/libfprint ./refs/libfprint/build/examples/enroll
   ```
3. Run the local `verify` example using `LD_LIBRARY_PATH`:
   ```bash
   sudo env LD_LIBRARY_PATH=refs/libfprint/build/libfprint ./refs/libfprint/build/examples/verify
   ```

For more details on local verification steps, see [plan-enroll-verify.md](file:///home/championswimmer/Development/Cpp/libfprint-eh577/docs/plan-enroll-verify.md).

---

## 4. Verify System-Wide Integration

Once system-installed, test the driver using the standard system-wide utilities or the Ubuntu UI:

### A. Terminal CLI Testing
```bash
# Try to enroll your fingerprint system-wide
fprintd-enroll

# Try to verify it
fprintd-verify
```

### B. Ubuntu GUI Settings
Open Ubuntu **Settings -> Users -> Fingerprint Login** to set up and test your fingerprint credentials.

---

## 5. How to Roll Back

If you encounter issues and want to revert to the official Ubuntu stock `libfprint` package:

### Option A: Using the Backup
```bash
sudo mv /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0.bak /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
sudo mv /lib/udev/rules.d/70-libfprint-2.rules.bak /lib/udev/rules.d/70-libfprint-2.rules 2>/dev/null || true
sudo systemctl restart fprintd
```

### Option B: Reinstalling via APT (Cleanest & Safest)
Reinstalling the package using Ubuntu's package manager will automatically restore the official libraries and override the custom build:

```bash
sudo apt install --reinstall libfprint-2-2 fprintd
```
