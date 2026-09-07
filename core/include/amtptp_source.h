#ifndef AMTPTP_SOURCE_H
#define AMTPTP_SOURCE_H
#include "amtptp_core.h"

#define AMTPTP_MODE_MOUSE 0u
#define AMTPTP_MODE_TOUCHPAD 3u
#define AMTPTP_HIDP_INPUT 0xa1u
#define AMTPTP_HIDP_SET_FEATURE 0x53u
#define AMTPTP_SOURCE_IGNORED 0
#define AMTPTP_SOURCE_REPORT 1
#define AMTPTP_SOURCE_INVALID (-1)

typedef struct amtptp_report_batch {
    amtptp_u8 count;
    amtptp_u8 reports[2][AMTPTP_PTP_REPORT_SIZE];
} amtptp_report_batch;

/* All access is serialized by the source driver's per-device lock. */
typedef struct amtptp_source {
    amtptp_session session;
    amtptp_options options;
    amtptp_frame previous;
    amtptp_u8 input_mode;
    amtptp_u8 surface_enabled;
    amtptp_u8 button_enabled;
    amtptp_u8 release_pending;
} amtptp_source;

extern const amtptp_u8 amtptp_source_descriptor[];
extern const amtptp_size amtptp_source_descriptor_size;
extern const amtptp_u8 amtptp_enable_multitouch[4];

void amtptp_source_init(amtptp_source *source);
int amtptp_source_get_feature(const amtptp_source *source, amtptp_u8 id,
    amtptp_u8 *buffer, amtptp_size capacity, amtptp_size *written);
int amtptp_source_set_feature(amtptp_source *source, amtptp_u8 id,
    const amtptp_u8 *buffer, amtptp_size length);
int amtptp_source_input(amtptp_source *source, const amtptp_u8 *sdu,
    amtptp_size length, amtptp_report_batch *batch);
/* Releases preserve IDs/positions before an empty frame, preventing stuck touches. */
void amtptp_source_release(amtptp_source *source, amtptp_report_batch *batch);
int amtptp_source_handshake(const amtptp_u8 *sdu, amtptp_size length);
/* Complete HIDP DATA/Input 0x90: [A1][90][status][percent]. Status flags
 * are retained as raw data; their charging interpretation is not assumed. */
int amtptp_source_battery(const amtptp_u8 *sdu, amtptp_size length,
    amtptp_u8 *percent, amtptp_u8 *flags);
#endif
