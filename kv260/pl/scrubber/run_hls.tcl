# Vitis HLS 2022.1: C simulation, synthesis and IP export of the scrubber.
#   vitis_hls -f run_hls.tcl
# Optional environment overrides:
#   FACESHIELD_PPC     pixels per clock on the video stream (default 2)
#   FACESHIELD_CLK_NS  video clock period in ns (default 3.333 = 300 MHz)
# Output IP: faceshield_scrubber_prj/solution1/impl/ip (add to Vivado's IP repo).

set ppc    [expr {[info exists ::env(FACESHIELD_PPC)] ? $::env(FACESHIELD_PPC) : 2}]
set clk_ns [expr {[info exists ::env(FACESHIELD_CLK_NS)] ? $::env(FACESHIELD_CLK_NS) : 3.333}]

open_project -reset faceshield_scrubber_prj
set_top faceshield_scrubber
add_files scrubber.cpp -cflags "-DFACESHIELD_PPC=$ppc"
add_files scrubber_core.hpp
add_files -tb tb_scrubber.cpp

open_solution -reset solution1 -flow_target vivado
set_part xck26-sfvc784-2LV-c
create_clock -period $clk_ns -name default

csim_design
csynth_design
export_design -format ip_catalog -vendor faceshield -library hls \
  -display_name "FaceShield fail-closed scrubber" -version 1.0
exit
