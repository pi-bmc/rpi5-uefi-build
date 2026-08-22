/** @file
 *
 *  Secondary System Description Table (SSDT) for the SoC thermal zone
 *
 *  Reads the BCM2712 AVS monitor's AVS_RO_TEMP_STATUS register (AVS block
 *  base 0x10_7D54_2000 + 0x200) directly from AML.
 *
 *  The linear conversion coefficients (slope -550, offset 450000, in
 *  milli-degrees Celsius per the brcm,bcm2711-thermal Linux binding) are
 *  shipped by the VideoCore firmware in the DTB 'coefficients' property:
 *
 *    milli-C = 450000 - 550 * raw
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

DefinitionBlock ("SsdtThermal.aml", "SSDT", 2, "RPIFDN", "RPI5THRM", 2)
{
  Scope (\_SB_)
  {
    ThermalZone (TZ00)
    {
      //
      // AVS_RO_TEMP_STATUS: bit 16 and bit 10 are validity flags,
      // bits 9:0 the raw temperature code.
      //
      OperationRegion (AVSM, SystemMemory, 0x107D542200, 4)
      Field (AVSM, DWordAcc, NoLock, Preserve)
      {
        TMPS, 32
      }

      Method (_TMP, 0, Serialized)
      {
        Local0 = TMPS

        //
        // Both validity bits (16 and 10) must be set; otherwise report a
        // safe ambient 25.0 C.
        //
        If ((Local0 & 0x10400) != 0x10400)
        {
          Return (2982)
        }

        Local1 = Local0 & 0x3FF       // raw temperature code
        Local2 = 550 * Local1         // subtrahend, in milli-C

        //
        // milli-C = 450000 - Local2. AML integer math is unsigned and
        // raw <= 0x3FF makes -112.65 C reachable, so clamp sub-zero
        // results to 0 C instead of wrapping.
        //
        If (Local2 >= 450000)
        {
          Return (2732)
        }

        Local3 = (450000 - Local2) / 100  // deci-C

        //
        // Cap absurd positive readings (raw near zero decodes to up to
        // +450 C) at 150.0 C.
        //
        If (Local3 > 1500)
        {
          Local3 = 1500
        }

        Return (2732 + Local3)        // deci-Kelvin
      }

      Name (_CRT, 3632)               // critical trip: 90.0 C
      Name (_PSV, 3582)               // passive trip: 85.0 C
      Name (_TZP, 300)                // polling period: 30 s (tenths of s)
      Name (_STR, Unicode ("SoC Thermal Zone"))
    }
  }
}
