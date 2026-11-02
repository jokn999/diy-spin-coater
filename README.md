# DIY Spin Coater

## Project overview

This project presents the development of a low-cost, open-source spin coater specifically designed for coating three-dimensional optical elements, particularly additively manufactured lenses. The system combines a high-torque stepper motor, a vibration-dampening base and an ESP32-based standalone control unit to provide precise and repeatable rotational speed profiles.

The firmware enables the operator to configure deposition speed, spinning speed and duration. An initial low-speed deposition phase allows coating resin to be distributed over the optical surface before transitioning to a controlled, timed spinning phase. Up to eight parameter profiles can be stored directly in the ESP32's non-volatile memory.

### Background and motivation

High-resolution masked stereolithography (mSLA) offers an accessible alternative to conventional, expensive optical manufacturing techniques. However, layer-by-layer fabrication introduces surface imperfections (the staircase effect), while oxygen inhibition during UV polymerization can leave partially cured, light-scattering resin on the surface.

To improve optical quality, these imperfections can be addressed by applying a thin, homogeneous layer of the same resin used for printing. This preserves refractive-index continuity at the optical boundary while smoothing the surface. Achieving a uniform coating requires precise and repeatable rotational speed profiles.

Commercial spin coaters are primarily designed for thin, flat silicon wafers. Their limited sample clearance and comparatively low torque make them unsuitable for larger, three-dimensional optical elements.

The objective of this project is therefore to provide an affordable and accessible alternative, combining readily available electronics, 3D-printed components and a vibration-dampening mechanical design. The resulting system was developed and characterized using additively manufactured plano-convex lenses made from Formlabs Clear V5 resin.

This repository contains the ESP32 firmware responsible for motor control, parameter configuration, profile management and the user interface. The associated project report documents the mechanical design, electronics, assembly and optical characterization.

## Features

- ESP32-based stepper-motor control with configurable initial and spin speeds.
- Two-stage operation: an initial low-speed phase followed by a timed spin phase.
- Eight parameter profiles, stored in ESP32 non-volatile preferences.
- SH1106 128×64 I²C OLED status display.
- Button-based configuration with debouncing and accelerated RPM adjustment when a button is held.
- Automatic controlled stop when the spin duration expires.

## Hardware

The sketch currently defines the following connections:

| Function | ESP32 GPIO |
|---|---:|
| OLED SDA | 22 |
| OLED SCL | 21 |
| Menu / selection button | 32 |
| Increase (+) button | 33 |
| Decrease (−) button | 25 |
| Stop button | 26 |
| Start button | 27 |
| Stepper STEP | 14 |
| Stepper DIR | 12 |
| Stepper ENABLE | 13 |

The buttons use `INPUT_PULLUP` and are therefore expected to connect the GPIO to GND when pressed. The display is configured as an SH1106 128×64 I²C module.

## Software setup

1. Install **Arduino IDE 2** and the ESP32 board support package.
2. Install the Arduino libraries **FastAccelStepper** and **U8g2**.
3. Open `diy_spin_coater.ino`, select the appropriate ESP32 board and serial port, then compile and upload.

`Arduino.h`, `Wire.h`, and `Preferences.h` are supplied by the ESP32 Arduino environment.

## Operation

In the home screen, **MENU** cycles through `Init RPM`, `Spin RPM`, `Time`, and `Profile`. Use **+ / −** to change the selected value.

- RPM changes by 5 RPM per press. Holding a button repeats the change every 120 ms: 25 RPM per repeat after 700 ms, and 100 RPM per repeat after 2 s.
- Time changes in 5-second increments. Profiles change once per button press and wrap around from 1 to 8.
- Press **START** once to run at the initial RPM. Press **START** again to ramp to the spin RPM and begin the timer.
- Press **STOP** to stop the motor from either running phase.
- At the end of the selected spin duration, the controller initiates a stop automatically.

The initial phase has no automatic timeout; a second START press is required to begin the timed phase.

## Default settings and limits

| Parameter | Default | Allowed range |
|---|---:|---:|
| Initial speed | 500 RPM | 0–5000 RPM |
| Spin speed | 2000 RPM | 0–5000 RPM |
| Spin duration | 30 s | 5–180 s |
| Profiles | 8 | — |
| Acceleration | 2000 RPM/s | Set in firmware |
| Steps per revolution | 1600 | Set in firmware |

The `MICROSTEPPING` value is used to convert RPM into step pulses. Configure the motor driver to match the effective number of step pulses per motor revolution.

## Safety

Verify the motor-driver wiring, logic levels, enable polarity, current limit, and step-pulse setting before powering the motor. Secure the motor, sample holder, and coating material, and use suitable physical guarding and eye protection. The software STOP and timed stop are controlled deceleration functions; they are **not** independent emergency-stop circuits. Do not leave the equipment unattended while running.

## Configuration

Hardware pins, speed and time limits, acceleration, button timing, and the step-pulse conversion are defined near the top of `diy_spin_coater.ino`. Profiles are saved automatically when edited; the last selected profile is restored after reboot.
