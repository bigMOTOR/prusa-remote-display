<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Parts and cost

Allow **about US$40–50** in consumed parts for one display, before shipping and
tax. This is a DIY build estimate in USD, checked **2 November 2026**, not a
selling price or a bundled kit quote. It assumes you already have access to a
3D printer and a computer. Prices and stock can change.

## Shopping list

| Part | Quantity | What to buy | Budget (USD) |
| --- | ---: | --- | ---: |
| Display board | 1 | [Waveshare ESP32-S3-LCD-2.8](https://www.waveshare.com/product/mcu-tools/development-boards/esp32-s3-touch-lcd-2.8.htm), **Without Touch Function** | $19.99–25.99 |
| PLA for the four case groups | Up to 200 g budget allowance | PLA and the [printing guide](../enclosure/printing.md) settings | $6.00 |
| USB data cable | 1 | Data + power; USB-C at the board end, computer-compatible at the other end | $4.95 |
| USB power supply | 1 | Regulated 5 V USB supply | $5.95 |
| M2.5×6 ISO 7380-1 button-head screws | 4 | Board mounting | Included in $5 fastener allowance |
| M3×8 ISO 7380-1 button-head screws | 2 | Rear cover | Included in $5 fastener allowance |
| Thin square M3 nuts | 2 | Cover nuts; check the [assembly guide](../enclosure/assembly.md) fit | Included in $5 fastener allowance |
| **Consumed-parts estimate** | | **$19.99–25.99 + $6 + $4.95 + $5.95 + $5** | **$41.89–47.89** |

The board manufacturer's page covers both touch and non-touch options; its
$19.99–25.99 listing range is a conservative family-price allowance, **not an
exact quote for the selected non-touch variant**. Select **Without Touch
Function** and check the final price. The word “touch” in the family-page URL
does not change the required board model. You may buy the exact non-touch model
from another seller.

The $6 print allowance is **0.2 kg × $29.99/kg, rounded up**. It is a buying
allowance for PLA, waste and small retries, not a measured or sliced model
weight. Use your own slicer's mass estimate for your printer and settings.
The [reference PLA price](https://www.prusa3d.com/en/product/prusament-pla-prusa-orange-1kg/)
is Prusament PLA Prusa Orange, $29.99 for 1 kg, listed in stock on the check
date; it is a price example, not a replacement for the printing guide's material
and setup recommendations.

The cable allowance uses [Adafruit 4474](https://www.adafruit.com/product/4474),
a USB-A to USB-C data/power cable listed at $4.95 and in stock. The power-supply
allowance uses [Adafruit 501](https://www.adafruit.com/product/501), 5 V / 1 A USB,
listed at $5.95 but out of stock on the check date.
[Newark's listing for the same item](https://www.newark.com/adafruit/501/psu-ac-dc-ite-1-outputs-5-25w/dp/44W4932)
was $5.83 and in stock. These are price references, not electrical validation
of a new power-supply or cable combination. Choose a supply with the appropriate
plug for your country and follow the board manufacturer's power requirements.

The $5 fastener amount is an **explicit budget allowance**, not a quoted basket.
As a price reference, [Metric Specialties DIN 562 thin square nuts](https://metricspecialties.com/collections/din-562)
were $0.11 each in zinc-plated steel or $0.35 in stainless steel.
[Accu's M2.5×6 button-head screw listing](https://www.accu.co.uk/socket-button-screws/8092-SSB-M2-5-6-A2)
provides a product reference; no verified USD screw price is used here. Check
head style and nut dimensions against the assembly guide rather than choosing
a fastener solely by its thread. Pack minimums and local availability may
require more than the allowance.

## Tools

| Tool | What it is for |
| --- | --- |
| Fine pliers | Grip the three temporary cassette pads during cleanup |
| 1.5 mm Allen key | M2.5 ISO 7380-1 button-head screws; use the required M2.5×6 length |
| 2 mm Allen key | M3×8 ISO 7380-1 button-head screws |

Socket-size references: [Accu M2.5 ISO 7380-1, 1.5 mm socket](https://www.accu.co.uk/socket-button-screws/155218-SSB-M2-5-8-A4-BL)
and [Accu M3×8 ISO 7380-1, 2 mm socket](https://www.accu.co.uk/socket-button-screws/8278-SSB-M3-8-A4).
The M2.5 reference page shows an 8 mm screw only as a socket-size reference;
**buy M2.5×6 for this build**, as listed above. Tools remain excluded from the
parts estimate; check the actual fasteners you buy.

## What the total excludes

Shipping, tax, import charges, printing service fees, electricity, labour, tools,
the printer and the computer are excluded. The total allocates only the PLA
used, not the full purchase of a spool; buying a new 1 kg spool and complete
fastener packs raises the initial checkout cost. Reusing a suitable data cable
and power supply saves about $11 from the estimate.

No soldering, glue or threaded inserts are required for this case assembly.
Follow [Get started](get-started.md) for assembly, installation and Wi-Fi setup.
