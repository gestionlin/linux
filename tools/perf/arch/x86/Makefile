# SPDX-License-Identifier: GPL-2.0
ifndef NO_DWARF
PERF_HAVE_DWARF_REGS := 1
endif
HAVE_KVM_STAT_SUPPORT := 1
PERF_HAVE_ARCH_REGS_QUERY_REGISTER_OFFSET := 1
PERF_HAVE_JITDUMP := 1

###
# Syscall table generation
#

generated := $(OUTPUT)arch/x86/include/generated
out       := $(generated)/asm
header    := $(out)/syscalls_64.c
header_32 := $(out)/syscalls_32.c
sys       := $(srctree)/tools/perf/arch/x86/entry/syscalls
systbl    := $(sys)/syscalltbl.sh

# Create output directory if not already present
$(shell [ -d '$(out)' ] || mkdir -p '$(out)')

$(header): $(sys)/syscall_64.tbl $(systbl)
	$(Q)$(SHELL) '$(systbl)' $(sys)/syscall_64.tbl 'x86_64' > $@

$(header_32): $(sys)/syscall_32.tbl $(systbl)
	$(Q)$(SHELL) '$(systbl)' $(sys)/syscall_32.tbl 'x86' > $@

clean::
	$(call QUIET_CLEAN, x86) $(RM) -r $(header) $(generated)

archheaders: $(header) $(header_32)
