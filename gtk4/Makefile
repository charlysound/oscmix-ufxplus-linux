.POSIX:

CC=cc
CPPFLAGS+=-I..
CFLAGS+=-std=c11 -O2 -g -Wall -Wextra -MMD -MP
GTK4_CFLAGS?=$(shell pkg-config --cflags gtk4)
GTK4_LDLIBS?=$(shell pkg-config --libs gtk4)
CFLAGS+=$(GTK4_CFLAGS)
LOCAL_APP_DIR?=$(HOME)/Applications/oscmix/oscmix-gtk4-prototype
LOCAL_DESKTOP_DIR?=$(HOME)/.local/share/applications

LOCAL_OBJ=main.o model.o transport.o mixer_row.o dsp_panel.o
OBJ=$(LOCAL_OBJ) ../osc.o ../device_ffufxp.o
DEPS=$(LOCAL_OBJ:.o=.d) model-test.d transport-test.d

-include $(DEPS)

all: check-version oscmix-gtk4

.PHONY: check-version
check-version:
	@pkg-config --atleast-version=4.6 gtk4 || { echo 'GTK 4.6 or newer is required' >&2; exit 1; }

oscmix-gtk4: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(GTK4_LDLIBS) -lm

../osc.o:
	$(MAKE) -C .. osc.o

../device_ffufxp.o:
	$(MAKE) -C .. device_ffufxp.o

model-test: model-test.o model.o ../device_ffufxp.o
	$(CC) $(LDFLAGS) -o $@ model-test.o model.o ../device_ffufxp.o $(GTK4_LDLIBS) -lm

transport-test: transport-test.o transport.o model.o ../osc.o ../device_ffufxp.o
	$(CC) $(LDFLAGS) -o $@ transport-test.o transport.o model.o ../osc.o ../device_ffufxp.o $(GTK4_LDLIBS) -lm

.PHONY: check
check: check-version model-test transport-test
	./model-test
	./transport-test

.PHONY: install-local
install-local: all
	$(MAKE) -C .. oscmix alsaseqio
	install -d $(LOCAL_APP_DIR)/bin $(LOCAL_DESKTOP_DIR)
	install -m 755 oscmix-gtk4 $(LOCAL_APP_DIR)/bin/oscmix-gtk4
	install -m 755 ../oscmix $(LOCAL_APP_DIR)/bin/oscmix
	install -m 755 ../alsaseqio $(LOCAL_APP_DIR)/bin/alsaseqio
	install -m 755 AppRun $(LOCAL_APP_DIR)/AppRun
	install -m 644 ../gtk/oscmix.svg $(LOCAL_APP_DIR)/oscmix.svg
	sed 's|@APPDIR@|$(LOCAL_APP_DIR)|g' oscmix-gtk4-prototype.desktop.in > $(LOCAL_DESKTOP_DIR)/oscmix-gtk4-prototype.desktop
	chmod 644 $(LOCAL_DESKTOP_DIR)/oscmix-gtk4-prototype.desktop
	-update-desktop-database $(LOCAL_DESKTOP_DIR)

.PHONY: clean
clean:
	rm -f oscmix-gtk4 model-test transport-test $(LOCAL_OBJ) model-test.o transport-test.o $(DEPS)
