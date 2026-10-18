# Fallback build when CMake is not installed.
#   make
#   make test
#   make clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iinclude
LDFLAGS ?=
LIBS ?= -pthread
UNAME_S := $(shell uname -s)
OPENSSL_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null)
ifneq ($(OPENSSL_LIBS),)
CPPFLAGS += -DCP_HAVE_OPENSSL=1 $(OPENSSL_CFLAGS)
LIBS += $(OPENSSL_LIBS)
endif
ifeq ($(UNAME_S),Linux)
CPPFLAGS += -DCP_HAVE_BLUEZ=1
LIBS += -lbluetooth
BLUEZ_SRC := src/platform/bluez_bluetooth.cpp
endif
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil libswscale libswresample 2>/dev/null)
FFMPEG_LIBS := $(shell pkg-config --libs libavcodec libavutil libswscale libswresample 2>/dev/null)
ifneq ($(FFMPEG_LIBS),)
CPPFLAGS += -DCP_HAVE_FFMPEG=1 $(FFMPEG_CFLAGS)
LIBS += $(FFMPEG_LIBS)
endif

SRC := \
  src/protocol/version.cpp \
  src/protocol/buffer_util.cpp \
  src/protocol/csm_framer.cpp \
  src/protocol/tlv8.cpp \
  src/protocol/iap_link.cpp \
  src/protocol/iap_accessory.cpp \
  src/protocol/mfi_signer.cpp \
  src/protocol/pair_setup.cpp \
  src/protocol/pair_verify.cpp \
  src/protocol/bplist.cpp \
  src/media/frame_clock.cpp \
  src/platform/null_bluetooth.cpp \
  src/platform/null_wifi.cpp \
  src/platform/posix_storage.cpp \
  src/platform/linux_system_info.cpp \
  src/platform/linux_hotspot.cpp \
  $(BLUEZ_SRC) \
  src/session/runtime_config.cpp \
  src/session/controller.cpp \
  src/session/receiver.cpp \
  src/session/control_session.cpp \
  src/session/session_services.cpp \
  src/session/audio_port.cpp \
  src/session/settings_io.cpp

OBJ := $(SRC:src/%.cpp=build/obj/%.o)
HDR := $(wildcard include/carplay/*.hpp) $(wildcard include/carplay/*/*.hpp)
QT_CFLAGS := $(shell pkg-config --cflags Qt5Widgets 2>/dev/null)
QT_LIBS := $(shell pkg-config --libs Qt5Widgets 2>/dev/null)
MOC := $(shell pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
ifeq ($(wildcard $(MOC)),)
MOC := moc
endif

.PHONY: all test ui clean

all: build/carplay-cli

build/carplay-cli: $(OBJ) build/obj/apps/cli/main.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

build/obj/%.o: src/%.cpp $(HDR)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/obj/apps/cli/main.o: apps/cli/main.cpp $(HDR)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/csm_framer_test: tests/csm_framer_test.cpp build/obj/protocol/csm_framer.o build/obj/protocol/buffer_util.o
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^

build/tlv8_test: tests/tlv8_test.cpp build/obj/protocol/tlv8.o build/obj/protocol/buffer_util.o
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^

build/iap_link_test: tests/iap_link_test.cpp build/obj/protocol/iap_link.o build/obj/protocol/iap_accessory.o build/obj/protocol/csm_framer.o build/obj/protocol/buffer_util.o
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^

build/system_info_test: tests/system_info_test.cpp build/obj/platform/linux_system_info.o
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^

test: build/csm_framer_test build/tlv8_test build/iap_link_test build/carplay-cli
	./build/csm_framer_test
	./build/tlv8_test
	./build/iap_link_test
	./build/carplay-cli doctor

build/moc_mainwindow.cpp: apps/ui-qt/mainwindow.hpp
	@mkdir -p build
	$(MOC) $< -o $@

build/moc_video_surface.cpp: apps/ui-qt/video_surface.hpp
	@mkdir -p build
	$(MOC) $< -o $@

build/obj/apps/ui-qt/%.o: apps/ui-qt/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(QT_CFLAGS) $(FFMPEG_CFLAGS) -fPIC -c $< -o $@

build/obj/apps/ui-qt/moc_mainwindow.o: build/moc_mainwindow.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(QT_CFLAGS) -fPIC -c $< -o $@

build/obj/apps/ui-qt/moc_video_surface.o: build/moc_video_surface.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(QT_CFLAGS) -fPIC -c $< -o $@

ui: build/carplay-ui

build/carplay-ui: $(OBJ) build/obj/apps/ui-qt/main.o build/obj/apps/ui-qt/mainwindow.o build/obj/apps/ui-qt/video_surface.o build/obj/apps/ui-qt/h264_decoder.o build/obj/apps/ui-qt/moc_mainwindow.o build/obj/apps/ui-qt/moc_video_surface.o
	@test -n "$(QT_LIBS)" || (echo "Qt5Widgets not found; install qtbase5-dev" && exit 1)
	@test -n "$(FFMPEG_LIBS)" || (echo "FFmpeg not found; install libavcodec-dev libswscale-dev libswresample-dev" && exit 1)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) $(QT_LIBS)

clean:
	rm -rf build
