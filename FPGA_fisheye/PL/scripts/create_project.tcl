# Vivado 2022.2: source this script or use -mode batch -source.
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root build]
if {[info exists ::env(FPGA_RACE_PL_BUILD)]} {
    set out [file normalize $::env(FPGA_RACE_PL_BUILD)]
}
if {[file exists [file join $out fisheye_mjpeg.xpr]]} {
    error "Project exists. Open it directly or set FPGA_RACE_PL_BUILD to a new directory."
}
create_project fisheye_mjpeg $out -part xczu7ev-ffvc1156-2-i
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]
set_property ip_repo_paths [list [file join $root fisheye_ip ip_repo]] [current_project]
update_ip_catalog
set_property verilog_define {FPGA_0_XILINX YUV444_ONLY} [current_fileset]
set_property include_dirs [list [file join $root mjpeg adapted] [file join $root mjpeg third_party core_jpeg_decoder src_v]] [current_fileset]
set source_paths {
    {mjpeg/rtl/axis_stream_mux.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_bitbuffer.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_core.v}
    {mjpeg/adapted/jpeg_decoder.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_dht.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_dht_std_cx_ac.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_dht_std_cx_dc.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_dht_std_y_ac.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_dht_std_y_dc.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_dqt.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_fifo.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_ram.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_ram_dp.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_transpose.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_transpose_ram.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_x.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_idct_y.v}
    {mjpeg/adapted/jpeg_input.v}
    {mjpeg/adapted/jpeg_mcu_id.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_mcu_proc.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_output.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_output_cx_ram.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_output_fifo.v}
    {mjpeg/third_party/core_jpeg/src_v/jpeg_output_y_ram.v}
    {mjpeg/rtl/mjpeg_rgb_burst.v}
    {mjpeg/rtl/mjpeg_decoder_axi.v}
    {mjpeg/adapted/jpeg_global.v}
    {mjpeg/adapted/ee_roms.sv}
    {mjpeg/adapted/fdct8.sv}
    {mjpeg/adapted/fdct_quant.sv}
    {mjpeg/adapted/header_gen.sv}
    {mjpeg/rtl/mjpeg_encoder_axis.sv}
    {mjpeg/adapted/quant.sv}
    {mjpeg/rtl/mjpeg_encoder_axis.v}
    {mjpeg/adapted/bitstream.sv}
    {mjpeg/rtl/camera.sv}
    {mjpeg/adapted/entropy_encoder.sv}
    {mjpeg/adapted/jpeg.sv}
    {mjpeg/adapted/misc.sv}
    {mjpeg/rtl/mjpeg_memories.sv}
    {mjpeg/third_party/core_jpeg_decoder/src_v/jpeg_decoder_input_fifo.v}
    {mjpeg/third_party/core_jpeg_decoder/src_v/jpeg_decoder_output_fifo.v}
    {mjpeg/third_party/core_jpeg_decoder/src_v/jpeg_decoder_defs.v}
}
foreach rel $source_paths {add_files -norecurse [file join $root $rel]}
update_compile_order -fileset sources_1
set bd_dir [file join $out fisheye_mjpeg.srcs sources_1 bd mjpeg_system]
file mkdir $bd_dir
set bd_file [file join $bd_dir mjpeg_system.bd]
file copy [file join $root design mjpeg_system.bd] $bd_file
add_files -norecurse $bd_file
open_bd_design $bd_file
validate_bd_design
save_bd_design
generate_target all [get_files $bd_file]
set wrapper [make_wrapper -files [get_files $bd_file] -top]
add_files -norecurse $wrapper
set_property top mjpeg_system_wrapper [current_fileset]
update_compile_order -fileset sources_1
puts "FPGA_RACE_PROJECT_READY: [file join $out fisheye_mjpeg.xpr]"
puts "Next: run synthesis, implementation and bitstream generation in Vivado."
