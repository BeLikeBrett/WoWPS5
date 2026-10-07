/* The payload SDK's libunwind.a is built for a single image: it finds FDEs
 * between __eh_frame_hdr_start/__eh_frame_start and their _end symbols, which
 * the SDK's executable script (ldscripts/elf_x86_64.x) provides. Linking it
 * into ntdll.so needs the same bounds around ntdll.so's own unwind tables, so
 * _Unwind_Find_FDE() covers ntdll.so's code and nothing else. */
SECTIONS
{
    .eh_frame_hdr : {
        PROVIDE_HIDDEN(__eh_frame_hdr_start = .);
        KEEP(*(.eh_frame_hdr))
        PROVIDE_HIDDEN(__eh_frame_hdr_end = .);
    }
    .eh_frame : {
        PROVIDE_HIDDEN(__eh_frame_start = .);
        KEEP(*(.eh_frame))
        PROVIDE_HIDDEN(__eh_frame_end = .);
    }
}
INSERT AFTER .rodata;
