CXX ?= c++
PKGS = hyprland pixman-1 libdrm libinput wayland-server lua
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++23 -fPIC -fno-gnu-unique -Wall -Wextra -Wno-unused-parameter
PREFIX ?= $(HOME)/.local

all: hyprspaceflow.so

hyprspaceflow.so: src/Plugin.cpp src/Motion.hpp src/SpeedGain.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared src/Plugin.cpp -o $@.tmp $(shell pkg-config --libs lua)
	mv $@.tmp $@

build/motion-test: tests/motion.cpp src/Motion.hpp src/SpeedGain.hpp
	mkdir -p build
	$(CXX) -std=c++23 -O2 -g -Wall -Wextra -Werror tests/motion.cpp -o $@

test: build/motion-test
	./build/motion-test
	luac -p examples/hyprspaceflow.lua
	luac -p examples/nested.lua

install: hyprspaceflow.so
	install -Dm755 hyprspaceflow.so "$(DESTDIR)$(PREFIX)/lib/hyprspaceflow/hyprspaceflow.so.tmp"
	mv "$(DESTDIR)$(PREFIX)/lib/hyprspaceflow/hyprspaceflow.so.tmp" "$(DESTDIR)$(PREFIX)/lib/hyprspaceflow/hyprspaceflow.so"
	install -Dm644 examples/hyprspaceflow.lua "$(DESTDIR)$(PREFIX)/share/hyprspaceflow/hyprspaceflow.lua"

clean:
	rm -rf build hyprspaceflow.so

.PHONY: all test install clean
