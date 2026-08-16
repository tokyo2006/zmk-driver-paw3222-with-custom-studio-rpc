# zmk-driver-paw3222-with-custom-studio-rpc

ZMK module providing the PAW3222 optical mouse sensor driver with an
unofficial custom ZMK Studio RPC protocol for sensor info, diagnostics,
runtime settings, and raw register access.

The PAW3222 is a low-power optical *motion* sensor (reports delta X/Y) --
unlike the PMW3610 it has no pixel array, so there is no frame-capture or
surface-quality API.

## Usage

Add to `config/west.yml`:

```yaml
projects:
  - name: zmk-driver-paw3222-with-custom-studio-rpc
    remote: <your-fork-remote>
    revision: main
```

Enable in `config/<shield>.conf`:

```conf
CONFIG_PAW3222=y
CONFIG_INPUT=y
CONFIG_ZMK_POINTING=y

CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_PAW3222_STUDIO_RPC=y

CONFIG_ZMK_CUSTOM_SETTINGS=y
CONFIG_ZMK_PAW3222_CUSTOM_SETTINGS=y
```

Devicetree:

```dts
#include <zephyr/dt-bindings/input/input-event-codes.h>

&spi0 {
    trackball: trackball@0 {
        compatible = "cormoran,paw3222";
        reg = <0>;
        spi-max-frequency = <2000000>;
        irq-gpios = <&gpio0 15 GPIO_ACTIVE_LOW>;
        evt-type = <INPUT_EV_REL>;
        x-input-code = <INPUT_REL_X>;
        y-input-code = <INPUT_REL_Y>;
    };
};
```

## RPC surface

- `GetInfo` -- per-device info (ready, product id, cpi, settings id).
- `ReadDiagnostics` -- product id / motion / cpi.
- `ReadRegister` / `WriteRegister` -- raw register access.
- Runtime settings `cpi` / `force_awake` via zmk-feature-custom-settings.
