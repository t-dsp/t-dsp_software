/* plaits_flashmem.ld — relocate the whole Plaits engine (lib/TDspPlaits2) to
 * FLASH instead of ITCM.
 *
 * Teensy 4 runs all code from ITCM (fast RAM) by default: any function not
 * marked FLASHMEM lands in the .text.itcm output section (RAM1). Plaits adds
 * ~140 KB of engine code, which overflows RAM1 in the full mix-kit (BT +
 * S/PDIF resamplers already fill it).
 *
 * This INSERT fragment augments the Teensy linker script (it does NOT replace
 * it): it grabs every .text* input section from libTDspPlaits2.a into a FLASH
 * output section placed BEFORE .text.itcm, so ld assigns those functions to
 * flash first and the generic *(.text*) ITCM rule never sees them. Flash on the
 * IMXRT1062 is cached, and each 128-sample audio block leaves ~2.67 ms of
 * slack, so running Plaits from flash costs little (re-measured on hardware).
 *
 * Applied only to the teensy41_plaits env via plaits_flashmem.py.
 */
SECTIONS
{
  .text.plaits_flash :
  {
    /* Most Plaits code is templated/inline, so it's emitted into whichever
     * object includes the headers (e.g. main.cpp.o), not just the archive.
     * Match by mangled namespace (-ffunction-sections gives each function its
     * own .text.<mangled> section): ::plaits:: = _ZN6plaits, ::stmlib:: =
     * _ZN6stmlib. Plus the archive itself for the AudioSynthPlaits/Plaits2Sink
     * wrapper (global namespace). */
    *(.text._ZN6plaits*)
    *(.text._ZN6stmlib*)
    /* Plaits' const lookup tables (esp. the 97 KB wav_integrated_waves) are
     * .rodata, which Teensy's script otherwise parks in DTCM (RAM1) for speed.
     * They're read-only, so relocate them to flash too. */
    *(.rodata._ZN6plaits*)
    *(.rodata._ZN6stmlib*)
    *libTDspPlaits2.a:(.text* .rodata*)
  } > FLASH
} INSERT BEFORE .text.itcm;
