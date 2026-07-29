#ifndef MIXER_H
#define MIXER_H

#include <gtk/gtk.h>
#include <glib-object.h>

G_DECLARE_FINAL_TYPE(Mixer, mixer, OSCMIX, MIXER, GObject)

typedef void (*MixerCallback)(GValue *arg, guint len, gpointer data);

Mixer *mixer_new(void);
void mixer_bind(Mixer *, char *addr, GType type, gpointer ptr, const char *prop);
void mixer_send(Mixer *, const char *addr, GValue *val);
void mixer_connect(Mixer *, char *addr, MixerCallback func, gpointer ptr);
void mixer_disconnect_by_data(Mixer *, gpointer ptr);
void mixer_set_adjustment_range(GtkAdjustment *, double lower, double upper, double step);

#endif
