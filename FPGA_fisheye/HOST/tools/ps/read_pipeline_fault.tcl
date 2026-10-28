# Read-only registers after a fault; no processor reset or firmware download.
connect -url tcp:127.0.0.1:3121
after 1000
puts [targets]
targets -set -nocase -filter {name =~ "APU*"}
configparams force-mem-access 1
# XSCT 2022.2 embeds Tcl 8.5, which does not provide try/finally.
set read_result [catch {
    foreach base {0x80000000 0x80040000 0x80030000 0x80010000 0x80020000} {
        puts "REG_BASE=$base"
        puts [mrd $base 8]
    }
    puts "CAPS_AND_CONFIG"
    puts [mrd 0x80030030 16]
} read_message read_options]
catch {configparams force-mem-access 0}
catch {disconnect}
if {$read_result} {return -options $read_options $read_message}
