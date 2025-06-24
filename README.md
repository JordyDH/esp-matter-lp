# Espressif's SDK for Matter - Low Power Edition

[![Docker Image](https://github.com/espressif/esp-matter/actions/workflows/docker-image.yml/badge.svg)](https://github.com/espressif/esp-matter/actions/workflows/docker-image.yml)
[![Launchpad Deployment](https://github.com/espressif/esp-matter/actions/workflows/pages.yml/badge.svg)](https://github.com/espressif/esp-matter/actions/workflows/pages.yml)

## Introduction

This is a customized fork of Espressif's SDK for Matter, specifically optimized for low power operation on ESP32 series SoCs. It builds upon the [original Espressif Matter SDK](https://github.com/espressif/esp-matter/) and the [open source Matter SDK](https://github.com/project-chip/connectedhomeip/), while introducing power-saving features and optimizations suitable for battery-powered and energy-efficient Matter products.

The low power optimizations in this fork include:
- Enhanced sleep modes management
- Reduced wake-up frequency
- Optimized network communication patterns
- Power-efficient peripheral handling

Like the original SDK, it provides simplified APIs, commonly used peripherals, tools and utilities for security, manufacturing, and production, accompanied by exhaustive documentation. It includes rich production references aimed to simplify the development of energy-efficient Matter products.

[Supported Device Types](SUPPORTED_DEVICE_TYPES.md)

## Supported Matter specification versions

| Matter Specification Version |                              Supported Branch                             |
|:----------------------------:|:-------------------------------------------------------------------------:|
|             v1.0             | [release/v1.0](https://github.com/espressif/esp-matter/tree/release/v1.0) |
|             v1.1             | [release/v1.1](https://github.com/espressif/esp-matter/tree/release/v1.1) |
|             v1.2             | [release/v1.2](https://github.com/espressif/esp-matter/tree/release/v1.2) |
|             v1.3             | [release/v1.3](https://github.com/espressif/esp-matter/tree/release/v1.3) |
|             v1.4             | [release/v1.4](https://github.com/espressif/esp-matter/tree/release/v1.4) |
|     v1.5 (Ongoing effort)    |         [main](https://github.com/espressif/esp-matter/tree/main)         |

## Getting the repositories

This repository has been modified to include all necessary submodule content directly in the main repository, eliminating the need for submodule initialization. Simply clone this repository:

```bash
git clone <repository-url>
cd esp-matter
```

## Supported ESP-IDF versions

- For Matter projects development with this SDK, it is recommended to utilize ESP-IDF [v5.4.1](https://github.com/espressif/esp-idf/tree/v5.4.1).

## Low Power Implementation Details

This fork introduces several key changes to optimize for low power operation:

1. **Sleep Mode Optimization**: Enhanced deep sleep integration with Matter protocol handling
2. **Duty Cycle Management**: Configurable active/sleep cycles to balance responsiveness and power consumption
3. **Efficient Network Stack**: Modified networking components to minimize power consumption during connectivity
4. **Example Applications**: Provided reference implementations for common low-power device types

## Documentation

For a simplified explanation of Matter concepts and internals, please check out the [Espressif's Matter blog series](https://blog.espressif.com/matter-38ccf1d60bcd).

Refer to the [Programming Guide](https://docs.espressif.com/projects/esp-matter/en/latest/) for the original ESP-Matter documentation. Additional documentation specific to low-power operation can be found in the [docs/low-power](docs/low-power) directory.

## Matter Specifications
Download the Matter specification from [CSA's official site](https://csa-iot.org/developer-resource/specifications-download-request/)

---

<a href="https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-matter/launchpad.toml">
    <img alt="Try it with ESP Launchpad" src="https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png" width="250" height="70">
</a>
