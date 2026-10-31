# XSCT / Vitis 2022.2. This creates a fresh platform; it does not program hardware.
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root build]
if {[info exists ::env(FPGA_RACE_PS_WORKSPACE)]} {
    set out [file normalize $::env(FPGA_RACE_PS_WORKSPACE)]
}
set xsa [file normalize [file join $root .. PL hardware_export pl_project_snapshot.xsa]]
if {![file exists $xsa]} {error "Missing PL hardware handoff: $xsa"}
if {[file exists [file join $out 7ev_doublefish_platform]]} {
    error "Platform exists. Use a new FPGA_RACE_PS_WORKSPACE or update it in Vitis."
}
file mkdir $out
setws $out
platform create -name 7ev_doublefish_platform -hw $xsa -proc psu_cortexa53_0 -os standalone -arch 64-bit -fsbl-target psu_cortexa53_0 -out $out
platform active 7ev_doublefish_platform
domain active standalone_domain
bsp setlib -name xilffs -ver 4.8
bsp setlib -name lwip211 -ver 1.8
bsp config stdin psu_uart_0
bsp config stdout psu_uart_0
# GEM TX completion interrupts free PBUF_RAM. NO_SYS mutexes do not protect
# the heap against those frees while the main loop allocates another packet.
bsp config -append extra_compiler_flags "-DLWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT=1"
bsp write
platform write
platform generate
puts "FPGA_RACE_PLATFORM_READY: $out"
puts "Create an Empty Application on standalone_domain and import PS/7ev_fish_app/src."
