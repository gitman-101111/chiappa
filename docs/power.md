# Power — charging, budget, and pitfalls (chiappa)

Hard-earned notes on the power subsystem. Any custom OS must handle these or
it will slowly drain the battery **while plugged in** and eventually hit a
hardware shutdown that looks like a brick (see the incident notes at the
bottom).

## Hardware

| Component | Role |
|---|---|
| MAX77818 | charger + fuel gauge (`max77818-charger`, `max77818-fuelgauge` / `max77818_battery` power_supply) |
| FUSB303B | USB Type-C CC controller (source-capability detection) |
| Battery | ~3 Ah class; hardware protection shutdown at **5% SOC** (`reboot: HARDWARE PROTECTION shutdown (Battery critical capacity)`) |

## The 500 mA trap

Without OS-side handling, the charger input current sits at the USB default:

```
fusb303b 1-0021: Adjusting USB current to 500mA
```

500 mA @ 5 V = **2.5 W ceiling** (minus charging losses). The stock OS
negotiates higher input current based on the Type-C CC advertisement / PD; a
custom OS that skips this will charge painfully slowly on wall bricks and can
run a **net drain on PC ports**. Check and manage
`/sys/class/power_supply/*/input_current_limit` against what the FUSB303B
detects.

## The idle-draw trap

The software TCON (`libqsgepaper` / SWTCON) runs a realtime generator thread
driving the display pipeline at **85 Hz continuously** — even when nothing on
screen changes. Combined with WiFi/BT and no suspend policy, a "idle" custom
OS draws ~2–3 W around the clock. The stock OS avoids this by pausing the
generator when no updates are pending and **auto-suspending after idle**.

A custom OS should:

1. **Auto-suspend on idle.** The panel is bistable — it holds the last image
   with zero power, so suspend is visually free. `echo mem > /sys/power/state`
   works; the power button wakes.
2. **Idle the waveform engine** when no updates are pending, if driving the
   engine directly.
3. **Shut down gracefully at ~8% SOC** (with a "battery empty" frame on the
   panel) before the 5% hardware cliff, which is an abrupt power cut.

## Interaction with A/B boot counters (the "plugged-in brick")

U-Boot increments the active slot's error counter each boot attempt and rolls
back to the other slot after 3 unreset failures. The OS resets the counter to
signal "boot succeeded" — **do this as late as possible in boot** (after the
device is reachable), not in an early service. If the reset runs early, a
mid-boot failure still counts as success and the rollback safety net is dead.

Failure mode observed with a flat battery: the device browns out mid-boot,
over and over, each attempt resetting the counter early → U-Boot never rolls
back → the device appears bricked while plugged in. Recovery required the SDP
slot-switch procedure ([recovery.md](recovery.md)).

## Incident summary (2026-07-02)

Device sat "charging" on a 500 mA PC port overnight with the display engine
running: draw exceeded supply → slow drain from full to 5% → hardware
protection shutdown → brownout boot loop masked by the early counter reset.
Filesystems survived (ext4 journaling); nothing was actually damaged. Fixes:
late boot-ok counter reset, idle/suspend policy, input-current negotiation,
graceful low-battery shutdown — all listed above.
