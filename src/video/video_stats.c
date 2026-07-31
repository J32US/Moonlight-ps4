#include "video.h"

#include <pthread.h>
#include <string.h>

static video_stats_t s_stats;
static pthread_mutex_t s_stats_lock = PTHREAD_MUTEX_INITIALIZER;

void video_get_stats(video_stats_t *out) {
    pthread_mutex_lock(&s_stats_lock);
    *out = s_stats;
    pthread_mutex_unlock(&s_stats_lock);
}

void video_reset_stats(void) {
    pthread_mutex_lock(&s_stats_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    pthread_mutex_unlock(&s_stats_lock);
}

void video_stats_add(unsigned long long decode_us, unsigned long long convert_us,
                     unsigned long long present_us, unsigned dropped) {
    pthread_mutex_lock(&s_stats_lock);
    s_stats.decode_us_total += decode_us;
    s_stats.convert_us_total += convert_us;
    s_stats.bgra_us_total += convert_us;
    s_stats.present_us_total += present_us;
    if (!dropped)
        s_stats.frames++;
    else
        s_stats.dropped += dropped;
    pthread_mutex_unlock(&s_stats_lock);
}

void video_stats_add_decode(unsigned long long decode_us) {
    pthread_mutex_lock(&s_stats_lock);
    s_stats.decode_us_total += decode_us;
    s_stats.decodes++;
    pthread_mutex_unlock(&s_stats_lock);
}

void video_stats_add_bounce(unsigned long long bounce_us) {
    pthread_mutex_lock(&s_stats_lock);
    s_stats.convert_us_total += bounce_us;
    s_stats.bounce_us_total += bounce_us;
    pthread_mutex_unlock(&s_stats_lock);
}
