# Test programs built with the open-source ps2sdk (https://github.com/ps2dev).
# They exercise the runtime's kernel and IOP service HLE through the same
# EE-side libraries (libkernel, libpad, libmc, libcdvd, libsdr) that use the
# standard Sony IOP module protocols.  Prebuilt ELFs are committed in bin/ so
# CI does not need the PS2 toolchain:  make -C tests/ps2sdk  (needs $PS2SDK)
TESTS = hello threads pad mc cdvd sound mpeg
EE_BIN_DIR = bin
all: $(addprefix $(EE_BIN_DIR)/,$(addsuffix .elf,$(TESTS)))

EE_CC = mips64r5900el-ps2-elf-gcc
EE_INCS = -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include
EE_CFLAGS = -D_EE -G0 -O2 -Wall -Wno-unused
EE_LDFLAGS = -L$(PS2SDK)/ee/lib -Wl,-zmax-page-size=128 -T$(PS2SDK)/ee/startup/linkfile
EE_LIBS_mpeg = -lmpeg -ldma -lm
EE_LIBS = -lpad -lmc -lsdr -lcdvd -lpatches -ldebug


$(EE_BIN_DIR)/%.elf: %.c
	@mkdir -p $(EE_BIN_DIR)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) $(EE_LDFLAGS) -o $@ $< $(EE_LIBS_$*) $(EE_LIBS)

clean:
	rm -rf $(EE_BIN_DIR)
