// PS4 audio renderer: decodes Opus with libopus and outputs via
// sceAudioOut in 256-sample blocks at 48 kHz.
#pragma once

#include <Limelight.h>

extern AUDIO_RENDERER_CALLBACKS audio_callbacks_orbis;
