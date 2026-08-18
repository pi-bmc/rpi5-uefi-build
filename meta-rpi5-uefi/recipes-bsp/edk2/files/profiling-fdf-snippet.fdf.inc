# Dp shell command, sed-included into RPi5.fdf's FVMAIN by the
# edk2-rpi5-firmware recipe. Mirrors profiling-dsc-snippet.inc -- see its
# header for why the gate is the PROFILING_ENABLED macro rather than the
# presence of these lines, and for what the platform already provides.
#
# A module built in the .dsc but missing here never dispatches, so the two
# snippets must stay in step.

!if $(PROFILING_ENABLED) == TRUE
  INF ShellPkg/DynamicCommand/DpDynamicCommand/DpDynamicCommand.inf
!endif
