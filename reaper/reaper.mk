REAPER_TARBALL_NAME := reaper784_linux_x86_64.tar.xz
REAPER_URL := https://www.reaper.fm/files/7.x/$(REAPER_TARBALL_NAME)
REAPER_SHA256 := d87b39862bec6a82bbb530731ae1fa36bba4bc9954ff72518a187420eed9ec7d
WDL_REPOSITORY := https://github.com/justinfrankel/WDL.git
WDL_COMMIT := 03c6e564b881a41cb13441cfd7350e92ae1ddf4d

THIRD_PARTY := $(BUILD)/third_party
REAPER_TARBALL := $(THIRD_PARTY)/$(REAPER_TARBALL_NAME)
REAPER_DIRECTORY := $(THIRD_PARTY)/reaper_linux_x86_64/REAPER
REAPER_STAMP := $(THIRD_PARTY)/reaper.stamp
WDL_DIRECTORY := $(THIRD_PARTY)/WDL
WDL_STAMP := $(THIRD_PARTY)/wdl.stamp

SWELL_BUILD := $(BUILD)/swell
SWELL_LIBRARY := $(SWELL_BUILD)/libSwell.so
SWELL_SOURCE := $(WDL_DIRECTORY)/WDL/swell
LICE_SOURCE := $(WDL_DIRECTORY)/WDL/lice
SWELL_NAMES := swell swell-ini swell-miscdlg-generic swell-wnd-generic swell-menu-generic swell-kb-generic \
	swell-dlg-generic swell-gdi-generic swell-misc-generic swell-gdi-lice swell-appstub-generic swell-modstub-generic
LICE_NAMES := lice lice_arc lice_colorspace lice_line lice_text lice_textnew lice_ico lice_bmp
SWELL_OBJECTS := $(addprefix $(SWELL_BUILD)/, $(addsuffix .o, $(SWELL_NAMES) $(LICE_NAMES)) swell-generic-fbdev.o)
SWELL_FLAGS := -O2 -pipe -fPIC -DPIC -fvisibility=hidden -fno-math-errno -fstrict-aliasing -D_FILE_OFFSET_BITS=64 \
	-DNDEBUG -DSWELL_MAKING_DYLIB -DSWELL_LICE_GDI -DSWELL_FREETYPE $(shell pkg-config --cflags freetype2) \
	-I$(SWELL_SOURCE) -w

$(REAPER_TARBALL):
	mkdir -p $(THIRD_PARTY)
	curl -fL --retry 3 -o $@.partial $(REAPER_URL)
	echo "$(REAPER_SHA256)  $@.partial" | sha256sum -c -
	mv $@.partial $@

$(REAPER_STAMP): $(REAPER_TARBALL)
	tar -C $(THIRD_PARTY) -xf $<
	touch $@

$(WDL_STAMP):
	mkdir -p $(THIRD_PARTY)
	rm -rf $(WDL_DIRECTORY)
	git init -q $(WDL_DIRECTORY)
	git -C $(WDL_DIRECTORY) fetch -q --depth 1 $(WDL_REPOSITORY) $(WDL_COMMIT)
	git -C $(WDL_DIRECTORY) checkout -q FETCH_HEAD
	touch $@

$(SWELL_BUILD)/%.o: $(SWELL_SOURCE)/%.cpp $(WDL_STAMP)
	mkdir -p $(SWELL_BUILD)
	$(CXX) $(SWELL_FLAGS) -c $< -o $@

$(SWELL_BUILD)/%.o: $(LICE_SOURCE)/%.cpp $(WDL_STAMP)
	mkdir -p $(SWELL_BUILD)
	$(CXX) $(SWELL_FLAGS) -c $< -o $@

$(SWELL_BUILD)/swell-generic-fbdev.o: reaper/swell-generic-fbdev.cpp $(WDL_STAMP)
	mkdir -p $(SWELL_BUILD)
	$(CXX) $(SWELL_FLAGS) -c $< -o $@

$(SWELL_LIBRARY): $(SWELL_OBJECTS)
	$(CXX) -shared -Wl,--no-undefined -s -o $@ $^ $(shell pkg-config --libs freetype2) -lpthread -ldl -lm

$(ROOTFS): $(REAPER_STAMP) $(SWELL_LIBRARY) tools/build_rootfs.py $(wildcard reaper/config/*)
	python3 tools/build_rootfs.py --reaper $(REAPER_DIRECTORY) --swell $(SWELL_LIBRARY) --config reaper/config --output $@
