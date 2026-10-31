# Temporary JTAG loading only. Does not write boot flash or change PS init.
# With a third FSBL argument, perform the Vitis JTAG initialization sequence.
# Without it, requires initialized DDR/clocks. HOST must be disconnected.
if {[llength $argv] < 2 || [llength $argv] > 3} {error "usage: load_debug_pipeline.tcl <bitstream> <elf> ?fsbl?"}
set debug_bit [file normalize [lindex $argv 0]]
set debug_elf [file normalize [lindex $argv 1]]
foreach p [list $debug_bit $debug_elf] {if {![file exists $p]} {error "missing file: $p"}}
connect -url tcp:127.0.0.1:3121
if {[llength $argv] == 3} {
    set debug_fsbl [file normalize [lindex $argv 2]]
    if {![file exists $debug_fsbl]} {error "missing FSBL: $debug_fsbl"}
    targets -set -nocase -filter {name =~ "APU*"}
    rst -system
    after 3000
} else {
    targets -set -nocase -filter {name =~ "Cortex-A53 #0"}
    stop
    rst -processor -clear-registers
}
targets -set -nocase -filter {name =~ "PL"}
fpga -file $debug_bit
if {[llength $argv] == 3} {
    set debug_root [file normalize [file join [file dirname [info script]] ../../..]]
    targets -set -nocase -filter {name =~ "APU*"}
    loadhw -hw [file join $debug_root PL/hardware_export/pl_project_snapshot.xsa] -regs
    configparams force-mem-access 1
    targets -set -nocase -filter {name =~ "Cortex-A53 #0"}
    rst -processor -clear-registers
    dow $debug_fsbl
    set debug_bp [bpadd -addr &XFsbl_Exit]
    set debug_rc [catch {con -block -timeout 45} debug_msg debug_opts]
    bpremove $debug_bp
    if {$debug_rc} {return -options $debug_opts $debug_msg}
    rst -processor -clear-registers
}
targets -set -nocase -filter {name =~ "Cortex-A53 #0"}
dow $debug_elf
configparams force-mem-access 0
con
puts "DEBUG_PIPELINE_LOADED: volatile PL configuration and PS RAM application only"
disconnect
