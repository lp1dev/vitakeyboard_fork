# vitakeyboard_fork

A PS Vita kernel plugin (skprx) and userland wrapper that emulates a USB HID
keyboard. When activated, the Vita appears to a connected host as a standard
USB keyboard (PID `054c:1338`) and can inject keystrokes into the host.

This is a fork of [mswlandi/vitakeyboard](mswlandi/vitakeyboard) 
that would **absolutely not exist** without mswlandi's work.

with three architectural fixes to make it work better:

1. Cache flush before every DMA send (`ksceKernelDcacheCleanRange`)
2. Connection-state gating before sending any report
3. Correct `_start` signature as a weak alias of `module_start`

---

## Architecture overview

This fork is what the [quark](https://github.com/lp1dev/quark) engine 
uses to be able to send keyboard inputs.

Here is my current usage of this fork for reference:

```
┌──────────────────────┐  JS bindings  ┌──────────────────────┐
│       Quark JS       │ ────────────► │  Userland wrapper    │
│   (Duktape, in app)  │               │  (vitakeyboard.c)    │
└──────────────────────┘               └──────────┬───────────┘
                                                  │ syscalls
                                                  ▼
┌──────────────────────────────────────────────────────────────┐
│                    Kernel plugin (skprx)                      │
│                                                               │
│  ┌──────────────┐    single-slot queue    ┌──────────────┐  │
│  │ syscall      │ ────────────────────►   │ update_thread│  │
│  │ handlers     │  (hasPendingKey/mod/    │  10ms poll   │  │
│  └──────────────┘   pendingKey + mutex)   └──────┬───────┘  │
│                                                  │           │
│                                                  ▼           │
│                                          ┌──────────────┐    │
│                                          │ send_inputs  │    │
│                                          │ + cache flush│    │
│                                          └──────┬───────┘    │
└─────────────────────────────────────────────────┼────────────┘
                                                  ▼
                                          ┌──────────────┐
                                          │   UDCD       │
                                          │ interrupt-IN │
                                          │  endpoint    │
                                          └──────┬───────┘
                                                 │ USB
                                                 ▼
                                              [host PC]
```

The kernel plugin registers itself as a UDCD driver named `VITA_KEYBOARD`.
When `hidkeyboard_user_start` is invoked it deactivates Sony's default USB
drivers (MTP, PSPCommunication, Serial), starts the keyboard driver, and
activates the device with USB PID `0x1338`.

A background thread polls every 10ms for pending key presses. When a key is
queued, it sends a press report on the next tick and a release report on the
tick after. Critically, the report buffer is cache-flushed before each
`ksceUdcdReqSend` call to ensure the USB controller's DMA reads the current
buffer contents and not stale cached data.

---

## USB device specification

| Field             | Value                              |
|-------------------|-------------------------------------|
| Vendor ID         | `0x054c` (Sony)                    |
| Product ID        | `0x1338`                           |
| USB class         | HID (0x03)                         |
| Subclass          | 0x00 (not boot interface)          |
| Protocol          | 0x00 (not boot keyboard)           |
| Endpoints         | EP0 control, EP1 interrupt-IN     |
| Polling interval  | 1ms (`bInterval=0x01`)             |
| HID version       | 1.11                               |
| Speed             | High-Speed                         |

### Report format

The interrupt-IN endpoint sends 8-byte HID reports:

| Offset | Name      | Description                             |
|--------|-----------|-----------------------------------------|
| 0      | modifier  | Bitmask: Ctrl/Shift/Alt/Meta L/R        |
| 1      | reserved  | Always 0                                |
| 2      | key1      | Primary HID keycode                     |
| 3      | key2      | (unused, 0)                            |
| 4      | key3      | (unused, 0)                            |
| 5      | key4      | (unused, 0)                            |
| 6      | key5      | (unused, 0)                            |
| 7      | key6      | (unused, 0)                            |

The current implementation only uses `key1`; the other slots are reserved for
future N-key rollover support.

Modifier bitmask values (`KEY_MOD_*` in `ascii_to_usb_hid.h`):

| Constant         | Value | Meaning             |
|------------------|-------|---------------------|
| `KEY_MOD_LCTRL`  | 0x01  | Left Control        |
| `KEY_MOD_LSHIFT` | 0x02  | Left Shift          |
| `KEY_MOD_LALT`   | 0x04  | Left Alt            |
| `KEY_MOD_LMETA`  | 0x08  | Left Meta/Win/Cmd   |
| `KEY_MOD_RCTRL`  | 0x10  | Right Control       |
| `KEY_MOD_RSHIFT` | 0x20  | Right Shift         |
| `KEY_MOD_RALT`   | 0x40  | Right Alt / AltGr   |
| `KEY_MOD_RMETA`  | 0x80  | Right Meta          |

HID keycodes follow USB HID 1.11 Usage Page 0x07 (Keyboard/Keypad). See
`ascii_to_usb_hid.h` for the full table.

---

## Kernel plugin API (syscalls)

All kernel functions are exposed as syscalls and must be linked against
`libhidkeyboard_stub_weak.a` (or `_stub.a`) generated by `vita_create_stubs`.

### `int hidkeyboard_user_start(void)`

Activates USB keyboard mode. Deactivates the current USB driver (typically
MTP), stops competing drivers, starts the keyboard driver, and activates it
with PID `0x1338`. The host will enumerate the device as a USB HID keyboard
within ~1 second.

**Returns:**
- `0` on success
- `HIDKEYBOARD_ERROR_DRIVER_NOT_REGISTERED` if `module_start` did not run
- `HIDKEYBOARD_ERROR_DRIVER_ALREADY_ACTIVATED` if already started
- Negative SCE error code on UDCD failure

**Side effects:** disconnects any existing USB session (e.g. content
manager, MTP transfer in progress).

### `int hidkeyboard_user_stop(void)`

Deactivates keyboard mode and restores the default Vita USB behaviour
(MTP, PID `0x04e4`).

**Returns:**
- `0` on success
- `HIDKEYBOARD_ERROR_DRIVER_NOT_ACTIVATED` if not currently active

### `int HidKeyBoardSendModifierAndKey(char modifier, char key)`

Queues a single key press with an optional modifier mask. The press is
emitted on the next 10ms tick after the connection is established, followed
by a release on the tick after.

**Parameters:**
- `modifier` — bitwise OR of `KEY_MOD_*` constants (0 for no modifier)
- `key` — HID keycode (0x00–0xFF). 0 sends no key.

**Returns:** `0` always (currently). The actual transmission happens
asynchronously in the kernel thread.

**Queue semantics:** the kernel has a **single-slot queue**. If you call this
faster than the kernel thread can drain it (~20ms per character), later calls
overwrite earlier ones before they are transmitted. Userland code must space
calls by at least 20–30ms.

**Example:**
```c
// Send Ctrl+C
HidKeyBoardSendModifierAndKey(KEY_MOD_LCTRL, KEY_C);  // 0x01, 0x06
```

### `int HidKeyboardSendChar(unsigned short c)`

Queues a single character using a keyboard layout mapping (currently
hardcoded to pt-BR; see `layouts/`). For characters outside the layout map,
returns 0 without queuing anything.

**Parameters:**
- `c` — UTF-16 code unit

**Returns:** `0` always. Silently drops unsupported characters.

This function exists to handle non-ASCII characters via dead keys / AltGr
sequences. For ASCII-only use cases, prefer the lookup table in the userland
wrapper which is faster.

---

## Userland wrapper API (`keyboard_vita.c` / `vitakeyboard.h`)

The userland wrapper provides a higher-level API and handles inter-character
timing.

### `int vita_keyboard_init(void)`

Stops any existing keyboard session (ignoring errors) and starts a new one.
Safe to call repeatedly.

**Returns:** result of `hidkeyboard_user_start()`.

### `int vita_keyboard_shutdown(void)`

Stops the keyboard and restores default USB behaviour.

**Returns:** result of `hidkeyboard_user_stop()`.

### `int vita_keyboard_send_char(unsigned short c)`

Sends a single character. Routing rules:

| Character range                | Handling                              |
|--------------------------------|---------------------------------------|
| `\b` (0x08)                    | Backspace (HID 0x2A)                  |
| `\n` (0x0A), `\r` (0x0D)       | Enter (HID 0x28)                      |
| `\t` (0x09)                    | Tab (HID 0x2B)                        |
| ESC (0x1B)                     | Escape (HID 0x29)                     |
| DEL (0x7F)                     | Delete (HID 0x4C)                     |
| Printable ASCII (0x20–0x7E)    | `ascii_to_hid_key_map[c - 0x20]`     |
| Other                          | Falls through to `HidKeyboardSendChar`|

**Returns:** `0` on success, negative on failure.

**Does not delay.** This is fire-and-forget — the kernel will emit press and
release on its own timing. To send multiple characters, use
`vita_keyboard_send_string` or insert your own delays.

### `int vita_keyboard_send_string(const char* str)`

Sends a null-terminated string. Iterates over each byte and calls
`vita_keyboard_send_char`, with a delay between characters to let the kernel
drain its single-slot queue.

**Parameters:**
- `str` — null-terminated C string (must not be NULL)

**Returns:** `0` on success, `-1` if `str` is NULL, negative SCE error on
syscall failure.

**Timing:** `INTER_CHAR_DELAY_US` (default 50000μs = 50ms) between characters.
Increase if characters get dropped on slower thread scheduling; decrease for
faster typing.

### `int vita_keyboard_send_modifier_key(char modifier, char key)`

Direct passthrough to `HidKeyBoardSendModifierAndKey`. Useful for sending
shortcuts.

---

## taiHEN setup

The kernel plugin must be loaded by taiHEN at boot.

`ur0:/tai/config.txt`:

```
*KERNEL
ur0:tai/hidkeyboard.skprx
```

The entry must be under `*KERNEL`, not `*main`. The plugin conflicts with
any other module that controls UDCD; `vitastick.skprx` and `udcd_uvc.skprx`
must be disabled (commented out) for the keyboard to enumerate.

A reboot is required after changing the skprx or config.txt — taiHEN does
not reload kernel modules at runtime.

---

## Build

The skprx is built with VitaSDK. From the `skprx/` directory:

```sh
mkdir build && cd build
cmake ..
make
```

This produces `hidkeyboard.skprx` and stubs :
- `libhidkeyboard_stub.a`
- `libhidkeyboard_stub_weak.a`

Userland apps link against the weak stub:

```cmake
target_link_libraries(myapp hidkeyboard_stub_weak)
```

Make sure the userland app picks up the freshly generated stub, not a stale
copy in the source tree. Whenever exports change, regenerate the stubs and
recopy them to wherever your build expects them.

---

## Limitations

- **Single-slot queue.** Sending characters faster than the kernel can drain
  them causes drops. Mitigated by `INTER_CHAR_DELAY_US` in the userland
  wrapper.
- **One key at a time.** No N-key rollover, no simultaneous modifier+key+key.
  The HID report has 6 key slots but only `key1` is used.
- **No LED feedback.** Caps Lock / Num Lock state from the host is not read.
- **Layout is hardcoded.** Non-ASCII characters use the pt-BR layout table.
- **No release notifications.** Userland cannot know when a character has
  finished transmitting; the inter-character delay is a fixed guess.
- **Polling architecture.** The 10ms tick caps the maximum typing rate at
  ~50 characters/second. A future event-driven port (modelled on
  `vitastick`) would lift this limit.

---

## Troubleshooting

**Host receives press but never release (auto-repeat fires forever)**

The cache flush before `ksceUdcdReqSend` is missing or the wrong NID is being
linked. Verify `ksceKernelDcacheCleanRange(g_inputs, sizeof(g_inputs))` is
called in `send_inputs()` and that `<psp2kern/kernel/cpu.h>` is included.

**Module crashes at boot with `PC: 0x0`**

The `_start` symbol is wrong. It must be a weak alias of `module_start` with
the exact signature `int module_start(SceSize args, void *argp)`.

**Module loads but device doesn't enumerate (still PID `0x04e4`)**

A competing USB driver is active. Check `config.txt` for `vitastick.skprx`,
`udcd_uvc.skprx`, or other UDCD-using modules and disable them.

**Keys are dropped when sending a long string**

`INTER_CHAR_DELAY_US` is too low. Increase from 50000 to 75000 or 100000.

**App crashes on JS bindings call with `PC: 0x0`**

Userland is linked against an outdated stub that doesn't include the export
being called. Rebuild the skprx, copy the new `libhidkeyboard_stub_weak.a`
to wherever your app's CMakeLists expects it, then rebuild the app.
