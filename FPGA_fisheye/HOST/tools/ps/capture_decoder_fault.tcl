# Read-only capture of decoder registers and their current DDR JPEG inputs.
# Usage: xsct tools/ps/capture_decoder_fault.tcl <new-output-directory>
# Does not halt/reset CPUs, change peripheral registers, or download firmware.
if {[llength $argv] != 1} {error "usage: capture_decoder_fault.tcl <new-output-directory>"}
set out_dir [file normalize [lindex $argv 0]]
if {[file exists $out_dir]} {error "refusing to overwrite existing capture: $out_dir"}
file mkdir $out_dir
set log_file [open [file join $out_dir registers.txt] w]
proc log_line {message} {
    global log_file
    puts $message
    puts $log_file $message
    flush $log_file
}
proc number {word} {
    # mrd -value returns Tcl integers (decimal on XSCT 2022.2).
    return [expr {wide($word)}]
}
connect -url tcp:127.0.0.1:3121
targets -set -nocase -filter {name =~ "APU*"}
configparams force-mem-access 1
set read_result [catch {
    log_line [targets]
    foreach {name base words} {front 0x80000000 8 rear 0x80040000 8 corrector 0x80030000 28 dma 0x80010000 24 gpio 0x80020000 8} {
        log_line "$name registers"
        log_line [mrd $base $words]
        mrd -bin -file [file join $out_dir ${name}_registers.bin] $base $words
    }
    set front_regs [mrd -value 0x80000000 8]
    set rear_regs [mrd -value 0x80040000 8]
    foreach {name values} [list front $front_regs rear $rear_regs] {
        set length [expr {[number [lindex $values 0]] & 0xffffff}]
        set address [number [lindex $values 2]]
        if {$length < 4 || $length > 8388608 || $address < 64 || $address + $length + 64 >= 0x80000000 || ($address & 63)} {
            error "invalid $name input: address=$address length=$length"
        }
        log_line [format "%s JPEG address=0x%08X length=%u" $name $address $length]
        mrd -size b -bin -file [file join $out_dir ${name}.jpg] $address $length
        mrd -size b -bin -file [file join $out_dir ${name}_padding.bin] [expr {$address+$length}] 64
    }
    set front_length [expr {[number [lindex $front_regs 0]] & 0xffffff}]
    set rear_length [expr {[number [lindex $rear_regs 0]] & 0xffffff}]
    set front_address [number [lindex $front_regs 2]]
    set rear_address [number [lindex $rear_regs 2]]
    set front_block [expr {($front_length+127)&~63}]
    set rear_block [expr {($rear_length+127)&~63}]
    if {$rear_address == $front_address + $front_block} {
        set pair_length [expr {64+$front_block+$rear_block}]
        mrd -size b -bin -file [file join $out_dir input_pair.bin] [expr {$front_address-64}] $pair_length
        log_line "paired input bytes=$pair_length"
    } else {
        log_line "decoder sources do not identify the same contiguous paired object"
    }
    if {[mrd -value 0x80000000 8] ne $front_regs || [mrd -value 0x80040000 8] ne $rear_regs} {
        error "decoder state changed during capture; input identity is not stable"
    }
    log_line "CAPTURE_STABLE: decoder registers unchanged throughout capture"
} read_message read_options]
catch {configparams force-mem-access 0}
catch {disconnect}
close $log_file
if {$read_result} {return -options $read_options $read_message}
