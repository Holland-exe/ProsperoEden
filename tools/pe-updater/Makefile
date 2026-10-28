# pe-updater - downloads and installs the newest ProsperoEden test build (a payload).
# Reuses ProsperoEden's self-update helper pieces: the validated ZIP extractor, the file
# helpers, SHA-256 and miniz.
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

ROOT := ../..
HELPER := $(ROOT)/headless/self_update_helper
CHECK := $(ROOT)/headless/update_check
OUTPUT ?= $(ROOT)/build/pe-updater/pe-updater.elf
WORK := $(dir $(OUTPUT))
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/third_party -I$(HELPER) -I$(CHECK)
CFLAGS := -std=c11 -O2 -Wall -Wextra -I$(CHECK)
MINIZ_CFLAGS := -std=c11 -O2 -w
LDFLAGS := -Wl,--build-id=none -lSceNet -lSceSsl -lSceHttp2

OBJECTS := $(WORK)main.o $(WORK)archive.o $(WORK)files.o $(WORK)self_update_sha256.o \
	$(WORK)miniz.o $(WORK)miniz_tinfl.o $(WORK)miniz_tdef.o $(WORK)miniz_zip.o

.PHONY: all clean
all: $(OUTPUT)

$(WORK)main.o: main.cpp
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(WORK)%.o: $(HELPER)/%.cpp
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(WORK)self_update_sha256.o: $(CHECK)/self_update_sha256.c
	@mkdir -p -- $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<
MINIZ_OBJECTS := $(WORK)miniz.o $(WORK)miniz_tinfl.o $(WORK)miniz_tdef.o $(WORK)miniz_zip.o
$(MINIZ_OBJECTS): $(WORK)%.o: $(ROOT)/third_party/miniz/%.c
	@mkdir -p -- $(@D)
	$(CC) $(MINIZ_CFLAGS) -c -o $@ $<

$(OUTPUT): $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf -- $(WORK)
