# Inserts the FaceShield scrubber into the KV260 Smart Camera block design.
# UNTESTED: written without access to Vivado. Run it in Vivado 2022.1 with the
# smartcam platform's block design open (see ../README.md), then review the
# diagram before generating anything.
#
#   source insert_scrubber.tcl
#
# Before:  csiss/video_out ──────────────────────────────► frmbuf_wr  (/dev/video0)
# After:   csiss/video_out ──► tee ─┬─► faceshield_scrubber ─► frmbuf_wr (clean, /dev/video0)
#                                   └─► faceshield_raw_fb               (raw, DPU input)
#
# Expects the scrubber IP in the IP repo: run ../scrubber/run_hls.tcl first and
# add faceshield_scrubber_prj/solution1/impl/ip to the project's IP repository.

proc one {what objs} {
  if {[llength $objs] != 1} {
    error "expected exactly one $what, found [llength $objs]: $objs"
  }
  return [lindex $objs 0]
}

set csi [one "MIPI CSI-2 RX subsystem" \
  [get_bd_cells -hier -filter {VLNV =~ "xilinx.com:ip:mipi_csi2_rx_subsystem:*"}]]
set fb  [one "frame buffer write" \
  [get_bd_cells -hier -filter {VLNV =~ "xilinx.com:ip:v_frmbuf_wr:*"}]]
puts "FaceShield: CSI-2 RX = $csi, frame buffer = $fb"

set csi_out [get_bd_intf_pins $csi/video_out]
set fb_in   [get_bd_intf_pins $fb/s_axis_video]
set net     [one "net on $csi/video_out" [get_bd_intf_nets -of_objects $csi_out]]
if {[lsearch [get_bd_intf_pins -of_objects $net] $fb_in] < 0} {
  error "$csi/video_out does not drive $fb/s_axis_video directly; the design \
         differs from what this script expects. Insert the scrubber by hand."
}

# Stream geometry: the scrubber must be built with the same pixels per clock.
set bytes [get_property CONFIG.TDATA_NUM_BYTES $csi_out]
puts "FaceShield: video stream is $bytes bytes per beat.\
      Build the scrubber with FACESHIELD_PPC=[expr {$bytes / 2}] (2 bytes per pixel)."

# Clock and reset of the existing frame buffer: the new blocks share them.
set clk_net [one "frame buffer clock net" [get_bd_nets -of_objects [get_bd_pins $fb/ap_clk]]]
set rst_net [one "frame buffer reset net" [get_bd_nets -of_objects [get_bd_pins $fb/ap_rst_n]]]

# Create the new cells in the same hierarchy as the frame buffer.
current_bd_instance [file dirname $fb]

delete_bd_objs $net

set tee [create_bd_cell -type ip -vlnv xilinx.com:ip:axis_broadcaster:1.1 faceshield_tee]
set_property -dict [list \
  CONFIG.NUM_MI {2} \
  CONFIG.S_TDATA_NUM_BYTES $bytes CONFIG.M_TDATA_NUM_BYTES $bytes \
  CONFIG.HAS_TLAST {1} CONFIG.TUSER_WIDTH {1} \
  CONFIG.M00_TDATA_REMAP "tdata\[[expr {$bytes * 8 - 1}]:0\]" \
  CONFIG.M01_TDATA_REMAP "tdata\[[expr {$bytes * 8 - 1}]:0\]"] $tee

set scrub [create_bd_cell -type ip -vlnv faceshield:hls:faceshield_scrubber:1.0 faceshield_scrubber]

# Second frame buffer, configured exactly like the existing one, for raw frames.
set raw_fb [create_bd_cell -type ip -vlnv [get_property VLNV $fb] faceshield_raw_fb]
set cfg {}
foreach p [list_property $fb CONFIG.*] {
  lappend cfg $p [get_property $p $fb]
}
set_property -dict $cfg $raw_fb

connect_bd_intf_net $csi_out [get_bd_intf_pins $tee/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins $tee/M00_AXIS] [get_bd_intf_pins $scrub/s_axis_video]
connect_bd_intf_net [get_bd_intf_pins $scrub/m_axis_video] $fb_in
connect_bd_intf_net [get_bd_intf_pins $tee/M01_AXIS] [get_bd_intf_pins $raw_fb/s_axis_video]

connect_bd_net -net $clk_net [get_bd_pins $tee/aclk] [get_bd_pins $scrub/ap_clk] \
  [get_bd_pins $raw_fb/ap_clk]
connect_bd_net -net $rst_net [get_bd_pins $tee/aresetn] [get_bd_pins $scrub/ap_rst_n] \
  [get_bd_pins $raw_fb/ap_rst_n]

# AXI-Lite control for the scrubber and the raw frame buffer, and the raw
# frame buffer's memory port: hang them off the same interconnects as the
# existing frame buffer.
set fb_ctrl_net [get_bd_intf_nets -of_objects [get_bd_intf_pins $fb/s_axi_CTRL]]
set fb_mem_net  [get_bd_intf_nets -of_objects [get_bd_intf_pins $fb/m_axi_mm_video]]
set ctrl_master [get_bd_intf_pins -of_objects $fb_ctrl_net -filter {MODE == Master}]
set mem_slave   [get_bd_intf_pins -of_objects $fb_mem_net -filter {MODE == Slave}]
puts "FaceShield: control master $ctrl_master, memory slave $mem_slave"

apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
  -config [list Clk_master Auto Clk_slave Auto Clk_xbar Auto \
           Master [file dirname [file dirname $ctrl_master]] Slave Auto intc_ip New] \
  [get_bd_intf_pins $scrub/s_axi_ctrl]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
  -config [list Clk_master Auto Clk_slave Auto Clk_xbar Auto \
           Master [file dirname [file dirname $ctrl_master]] Slave Auto intc_ip New] \
  [get_bd_intf_pins $raw_fb/s_axi_CTRL]
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
  -config [list Clk_master Auto Clk_slave Auto Clk_xbar Auto \
           Master Auto Slave [file dirname [file dirname $mem_slave]] intc_ip Auto] \
  [get_bd_intf_pins $raw_fb/m_axi_mm_video]

assign_bd_address

puts "FaceShield: TODO by hand:"
puts "  1. Connect faceshield_raw_fb/interrupt to a free input of the interrupt concat"
puts "     that feeds the PS (pl_ps_irq), next to the existing frame buffer's interrupt."
puts "  2. Check the address map: note the scrubber's base address for the device tree."
puts "  3. Check that the scrubber was built with the pixels-per-clock printed above."
validate_bd_design
