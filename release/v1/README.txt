SymbianOverclock v1.0 - working version, proven on hardware 2026-08-08.

Achieved: DPLL multiplier 86 -> 111 at runtime, 330 -> 426 MHz (+29%),
memory bus stock, device stable, calls and WiFi working.
SPmark 3D: 62 -> 75 fps.

CM_CLKSEL1_CORE dividers are selected AUTOMATICALLY (L3 - as high as possible within
+15%, DSS1 - closest to stock). No manual mode - it appeared in v1.1.

Files here are an exact snapshot of what worked. To rebuild this exact version,
copy ocengine.h/ocengine.cpp/ocgui.cpp back into src/ and build with
tools/build_sis.cmd.
