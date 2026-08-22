CC = m68k-amigaos-gcc
STRIP = m68k-amigaos-strip
OBJDUMP = m68k-amigaos-objdump

PROBE_TARGET = AirPrintProbe
PREFS_TARGET = AmiAirPrintPrefs
PREFS_CLASSIC_TARGET = AmiAirPrintPrefsClassic
TEST_TARGET = AirPrintTest
CONFIG_TARGET = AirPrintConfig
DEVICE_TARGET = airprint.device
DEVICE_TEST_TARGET = AirPrintDeviceTest
PRINTER_TARGET = AirPrint
PRINTER_TEST_TARGET = AirPrintPrinterTest

CFLAGS = -m68000 -Os -Wall -Wextra -Wshadow -Wpointer-arith -Wframe-larger-than=1024 -DNO_INLINE_STDARG -Iinclude
LDFLAGS = -m68000
RUNTIME = -noixemul

DEVICE_CFLAGS = -m68000 -Os -Wall -Wextra -Wpointer-arith -Iinclude \
	-ffreestanding -fno-builtin -fomit-frame-pointer \
	-Wno-unused-parameter -Wno-volatile-register-var -Wframe-larger-than=1024
DEVICE_LDFLAGS = -m68000 -nostdlib -nostartfiles -Wl,-Map=$(DEVICE_TARGET).map

PRINTER_CFLAGS = -m68000 -Os -Wall -Wextra -Wpointer-arith -Iinclude \
	-ffreestanding -fno-builtin -fomit-frame-pointer -fno-toplevel-reorder \
	-Wno-unused-parameter -Wno-volatile-register-var -Wframe-larger-than=1024
PRINTER_LDFLAGS = -m68000 -nostdlib -nostartfiles -Wl,-Map=$(PRINTER_TARGET).map

CORE_OBJECTS = \
	src/airprint_ipp.o \
	src/airprint_http_amiga.o

PROBE_OBJECTS = \
	src/main.o \
	$(CORE_OBJECTS)

PREFS_OBJECTS = \
	src/prefs_main.o \
	src/ami_airprint_locale.o \
	src/airprint_discovery.o \
	src/airprint_print.o \
	src/testpage_jpeg.o \
	src/airprint_caps.o \
	src/airprint_prefs.o \
	$(CORE_OBJECTS)

PREFS_CLASSIC_OBJECTS = \
	src/prefs_gadtools_main.o \
	src/ami_airprint_locale.o \
	src/airprint_discovery.o \
	src/airprint_print.o \
	src/testpage_jpeg.o \
	src/airprint_caps.o \
	src/airprint_prefs.o \
	$(CORE_OBJECTS)

TEST_OBJECTS = \
	src/test_main.o \
	src/airprint_print.o \
	src/testpage_jpeg.o \
	src/airprint_caps.o \
	src/airprint_prefs.o \
	$(CORE_OBJECTS)

CONFIG_OBJECTS = \
	src/config_main.o \
	src/airprint_caps.o \
	src/airprint_prefs.o \
	$(CORE_OBJECTS)

DEVICE_TEST_OBJECTS = \
	src/device_test_main.o \
	src/testpage_jpeg.o

PRINTER_OBJECTS = \
	src/printertag.o \
	src/printerglue.o \
	src/mulsi3.o \
	src/airprint_printer.o

PRINTER_TEST_OBJECTS = \
	src/printer_test_main.o

REQUIRED_FILES = \
	include/ami_airprint_brand.h \
	include/ami_airprint_version.h \
	include/ami_airprint_locale.h \
	include/airprint_http.h \
	include/airprint_ipp.h \
	include/airprint_caps.h \
	include/airprint_discovery.h \
	include/airprint_prefs.h \
	include/airprint_print.h \
	include/airprint_device.h \
	include/airprint_text_font.h \
	include/testpage_jpeg.h \
	src/ami_airprint_locale.c \
	src/airprint_http_amiga.c \
	src/airprint_ipp.c \
	src/airprint_caps.c \
	src/airprint_discovery.c \
	src/airprint_prefs.c \
	src/airprint_print.c \
	src/airprint_device.c \
	src/testpage_jpeg.c \
	src/main.c \
	src/prefs_main.c \
	src/prefs_gadtools_main.c \
	src/test_main.c \
	src/config_main.c \
	src/device_test_main.c \
	src/printertag.s \
	src/printerglue.s \
	src/mulsi3.S \
	src/airprint_printer.c \
	src/printer_test_main.c \
	locale/AmiAirPrint.cd

.PHONY: all clean strip check-sources device-disasm printer-disasm

all: check-sources $(PROBE_TARGET) $(PREFS_TARGET) $(PREFS_CLASSIC_TARGET) $(TEST_TARGET) \
	$(CONFIG_TARGET) $(DEVICE_TARGET) $(DEVICE_TEST_TARGET) \
	$(PRINTER_TARGET) $(PRINTER_TEST_TARGET)

check-sources:
	@missing=0; \
	for f in $(REQUIRED_FILES); do \
		if [ ! -f "$$f" ]; then \
			echo "ERROR: required source file missing: $$f"; \
			missing=1; \
		fi; \
	done; \
	if [ $$missing -ne 0 ]; then \
		echo "Extract/copy the complete source package before running make."; \
		exit 1; \
	fi

$(PROBE_TARGET): $(PROBE_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(PROBE_OBJECTS) $(RUNTIME)

$(PREFS_TARGET): $(PREFS_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(PREFS_OBJECTS) $(RUNTIME)

$(PREFS_CLASSIC_TARGET): $(PREFS_CLASSIC_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(PREFS_CLASSIC_OBJECTS) $(RUNTIME)

$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJECTS) $(RUNTIME)

$(CONFIG_TARGET): $(CONFIG_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(CONFIG_OBJECTS) $(RUNTIME)

$(DEVICE_TEST_TARGET): $(DEVICE_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(DEVICE_TEST_OBJECTS) $(RUNTIME)

$(PRINTER_TEST_TARGET): $(PRINTER_TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(PRINTER_TEST_OBJECTS) $(RUNTIME)

src/prefs_main.o: src/prefs_main.c include/ami_airprint_version.h include/ami_airprint_locale.h include/airprint_discovery.h
	$(CC) $(CFLAGS) -Wno-pointer-sign -c -o $@ $< $(RUNTIME)

src/prefs_gadtools_main.o: src/prefs_gadtools_main.c include/ami_airprint_version.h include/ami_airprint_locale.h include/airprint_discovery.h
	$(CC) $(CFLAGS) -Wno-pointer-sign -c -o $@ $< $(RUNTIME)

src/airprint_discovery.o: src/airprint_discovery.c include/airprint_discovery.h include/airprint_http.h
	$(CC) $(CFLAGS) -c -o $@ $< $(RUNTIME)

src/ami_airprint_locale.o: src/ami_airprint_locale.c include/ami_airprint_locale.h
	$(CC) $(CFLAGS) -c -o $@ $< $(RUNTIME)

src/printertag.o: src/printertag.s
	$(CC) -m68000 -c -o $@ $<

src/printerglue.o: src/printerglue.s
	$(CC) -m68000 -c -o $@ $<

src/mulsi3.o: src/mulsi3.S
	$(CC) -m68000 -c -o $@ $<

src/airprint_printer.o: src/airprint_printer.c include/airprint_device.h include/airprint_text_font.h include/ami_airprint_brand.h include/ami_airprint_version.h
	$(CC) $(PRINTER_CFLAGS) -c -o $@ $<

src/printer_test_main.o: src/printer_test_main.c include/airprint_device.h include/ami_airprint_version.h
	$(CC) $(CFLAGS) -c -o $@ $< $(RUNTIME)


$(PRINTER_TARGET): $(PRINTER_OBJECTS)
	$(CC) $(PRINTER_CFLAGS) -o $@ $(PRINTER_OBJECTS) $(PRINTER_LDFLAGS)

src/airprint_device.o: src/airprint_device.c include/airprint_device.h include/ami_airprint_brand.h include/ami_airprint_version.h
	$(CC) $(DEVICE_CFLAGS) -c -o $@ $<

$(DEVICE_TARGET): src/airprint_device.o
	$(CC) $(DEVICE_CFLAGS) -o $@ $< $(DEVICE_LDFLAGS)

src/main.o src/test_main.o src/device_test_main.o src/config_main.o \
	src/airprint_http_amiga.o src/airprint_prefs.o: include/ami_airprint_version.h

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $< $(RUNTIME)

device-disasm: $(DEVICE_TARGET)
	$(OBJDUMP) -D $(DEVICE_TARGET) > $(DEVICE_TARGET).s

printer-disasm: $(PRINTER_TARGET)
	$(OBJDUMP) -D $(PRINTER_TARGET) > $(PRINTER_TARGET).s

strip: all
	$(STRIP) $(PROBE_TARGET)
	$(STRIP) $(PREFS_TARGET)
	$(STRIP) $(PREFS_CLASSIC_TARGET)
	$(STRIP) $(TEST_TARGET)
	$(STRIP) $(CONFIG_TARGET)
	$(STRIP) $(DEVICE_TEST_TARGET)
	$(STRIP) $(DEVICE_TARGET)
	$(STRIP) $(PRINTER_TARGET)
	$(STRIP) $(PRINTER_TEST_TARGET)

clean:
	rm -f src/*.o $(PROBE_TARGET) $(PREFS_TARGET) $(PREFS_CLASSIC_TARGET) AmiAirPrint AirPrintPrefs $(TEST_TARGET) \
		$(CONFIG_TARGET) $(DEVICE_TARGET) $(DEVICE_TEST_TARGET) \
		$(PRINTER_TARGET) $(PRINTER_TEST_TARGET) \
		$(DEVICE_TARGET).map $(DEVICE_TARGET).s $(PRINTER_TARGET).map $(PRINTER_TARGET).s
