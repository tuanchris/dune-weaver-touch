"""Pre-build hook: per-environment sdkconfig overrides.

PlatformIO seeds every env from the single project-wide `sdkconfig.defaults`
and offers no per-env defaults file, but the boards genuinely disagree on one
Kconfig setting: where the console goes.

  - Waveshare 5B / 5 / 7: native USB-Serial-JTAG. UART0 is NOT usable there --
    on the 5B it drives the RS485 transceiver.
  - Elecrow CrowPanel Advance 5: no USB-Serial-JTAG at all (GPIO19/20 are the
    I2S microphone), so the console has to be UART0, which is what the onboard
    CH340K bridges to USB-C.

Setting either one globally breaks the other, so each env declares what it
needs in platformio.ini:

    custom_sdkconfig =
        CONFIG_ESP_CONSOLE_UART_DEFAULT=y

Values land in the env's generated `sdkconfig.<env>`, which the IDF build
treats as authoritative over `sdkconfig.defaults` -- so they survive
regeneration. Lines for the same CONFIG key are replaced, not duplicated.
"""

import os
import re

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)


def main():
    raw = env.GetProjectOption("custom_sdkconfig", "")  # noqa: F821
    wanted = [ln.strip() for ln in raw.splitlines() if ln.strip()
              and not ln.strip().startswith("#")]
    if not wanted:
        return

    pioenv = env.subst("$PIOENV")  # noqa: F821
    path = os.path.join(env.subst("$PROJECT_DIR"), "sdkconfig.%s" % pioenv)  # noqa: F821

    lines = []
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as fh:
            lines = fh.read().splitlines()

    changed = False
    for entry in wanted:
        key = entry.split("=", 1)[0]
        # Match the setting itself and the "# CONFIG_X is not set" form.
        pat = re.compile(r"^(%s=|# %s is not set)" % (re.escape(key), re.escape(key)))
        for i, ln in enumerate(lines):
            if pat.match(ln):
                if ln != entry:
                    lines[i] = entry
                    changed = True
                break
        else:
            lines.append(entry)
            changed = True

    if changed:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        print("sdkconfig_env: applied %d override(s) to sdkconfig.%s"
              % (len(wanted), pioenv))


main()
