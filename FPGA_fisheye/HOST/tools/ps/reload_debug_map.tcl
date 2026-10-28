# Run with `source` in the Vitis XSCT Console after a failed launch.
# Refresh debugger metadata only; no reset, programming or application download.
set reload_host_root [file normalize [file join [file dirname [info script]] ../..]]
set reload_xsa [file normalize [file join $reload_host_root ../PL/hardware_export/pl_project_snapshot.xsa]]
if {![file exists $reload_xsa]} {error "Missing exported XSA: $reload_xsa"}
targets -set -nocase -filter {name =~ "APU*"}
unloadhw
loadhw -hw $reload_xsa -mem-ranges [list {0x80000000 0xbfffffff} {0x400000000 0x5ffffffff} {0x1000000000 0x7fffffffff}] -regs
# Leave no cached mapping for the default IDE launch, which loads XSA again.
unloadhw
puts "Debugger memory map checked and cleared. Retry the Vitis launch."
unset reload_host_root reload_xsa
