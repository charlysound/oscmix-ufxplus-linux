#ifndef OSCMIX_GTK4_MIXER_ROW_H
#define OSCMIX_GTK4_MIXER_ROW_H

#include <gtk/gtk.h>
#include "model.h"
#include "transport.h"

#define OSCMIX_TYPE_MIXER_ROW (oscmix_mixer_row_get_type())
G_DECLARE_FINAL_TYPE(OscmixMixerRow, oscmix_mixer_row, OSCMIX, MIXER_ROW, GtkWidget)

GtkWidget *oscmix_mixer_row_new(MixModel *model, OscTransport *transport,
	MixChannelKind kind);
GtkAdjustment *oscmix_mixer_row_get_adjustment(OscmixMixerRow *self);
void oscmix_mixer_row_set_editor(OscmixMixerRow *self,
	GtkWidget *editor, int channel);
void oscmix_mixer_row_release_editor(OscmixMixerRow *self);

#endif
