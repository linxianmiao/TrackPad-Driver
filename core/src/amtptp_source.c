#include "amtptp_source.h"
#include "amtptp_hqa.h"

/* HIDP SET_REPORT(feature), Apple report F1, multitouch mode. */
const amtptp_u8 amtptp_enable_multitouch[4] = {0x53u, 0xf1u, 0x02u, 0x01u};
static const amtptp_u8 hqa[256] = {DEFAULT_PTP_HQA_BLOB};

/* Reset global physical/unit state for every finger; all five slots match
 * the shared core's explicit 9-byte serialization, including 32-bit IDs. */
#define FINGER \
    0x05,0x0d,0x09,0x22,0xa1,0x02, \
    0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x00,0x55,0x00,0x65,0x00, \
    0x09,0x47,0x09,0x42,0x75,0x01,0x95,0x02,0x81,0x02, \
    0x95,0x06,0x81,0x03, \
    0x09,0x51,0x27,0xff,0xff,0xff,0xff,0x75,0x20,0x95,0x01,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x26,0xbc,0x1d,0x46,0x40,0x06, \
    0x55,0x0e,0x65,0x11,0x75,0x10,0x81,0x02, \
    0x09,0x31,0x26,0xc9,0x13,0x46,0x7d,0x04,0x81,0x02,0xc0

const amtptp_u8 amtptp_source_descriptor[] = {
    0x05,0x0d,0x09,0x05,0xa1,0x01,0x85,0x05,
    FINGER,FINGER,FINGER,FINGER,FINGER,
    0x05,0x0d,0x09,0x56,0x55,0x0c,0x66,0x01,0x10,
    0x27,0xff,0xff,0x00,0x00,0x47,0xff,0xff,0x00,0x00,
    0x75,0x10,0x95,0x01,0x81,0x02,
    0x55,0x00,0x65,0x00,0x35,0x00,0x45,0x00,
    0x09,0x54,0x25,0x05,0x75,0x08,0x81,0x02,
    0x05,0x09,0x09,0x01,0x25,0x01,0x75,0x01,0x81,0x02,
    0x95,0x07,0x81,0x03,
    0x05,0x0d,0x85,0x07,0x09,0x55,0x09,0x59,
    0x26,0xff,0x00,0x75,0x08,0x95,0x02,0xb1,0x02,
    0x06,0x00,0xff,0x85,0x08,0x09,0xc5,
    0x75,0x08,0x96,0x00,0x01,0xb1,0x02,0xc0,
    0x05,0x0d,0x09,0x0e,0xa1,0x01,
    0x85,0x04,0x09,0x22,0xa1,0x02,0x09,0x52,
    0x15,0x00,0x25,0x03,0x75,0x08,0x95,0x01,0xb1,0x02,0xc0,
    0x85,0x06,0x09,0x57,0x09,0x58,0x25,0x01,
    0x75,0x01,0x95,0x02,0xb1,0x02,0x95,0x06,0xb1,0x03,0xc0
};
const amtptp_size amtptp_source_descriptor_size = sizeof(amtptp_source_descriptor);

static void clear_frame(amtptp_frame *frame)
{
    amtptp_frame empty = {0};
    *frame = empty;
}

static void append_frame(amtptp_report_batch *batch, const amtptp_frame *frame)
{
    amtptp_size written;
    if (batch->count < 2u && amtptp_serialize_ptp(frame,
        batch->reports[batch->count], AMTPTP_PTP_REPORT_SIZE, &written) == AMTPTP_OK) {
        ++batch->count;
    }
}

void amtptp_source_init(amtptp_source *source)
{
    amtptp_default_options(&source->options);
    amtptp_reset_session(&source->session);
    clear_frame(&source->previous);
    source->input_mode = AMTPTP_MODE_MOUSE;
    source->surface_enabled = 1u;
    source->button_enabled = 1u;
    source->release_pending = 0u;
}

int amtptp_source_get_feature(const amtptp_source *source, amtptp_u8 id,
    amtptp_u8 *buffer, amtptp_size capacity, amtptp_size *written)
{
    amtptp_size needed, index;
    if (source == NULL || buffer == NULL || written == NULL) return AMTPTP_SOURCE_INVALID;
    *written = 0u;
    switch (id) {
    case 4: case 6: needed = 2u; break;
    case 7: needed = 3u; break;
    case 8: needed = 257u; break;
    default: return AMTPTP_SOURCE_INVALID;
    }
    if (capacity < needed) return AMTPTP_SOURCE_INVALID;
    for (index = 0u; index < capacity; ++index) buffer[index] = 0u;
    buffer[0] = id;
    if (id == 4u) buffer[1] = source->input_mode;
    if (id == 6u) buffer[1] = (amtptp_u8)(source->button_enabled | (source->surface_enabled << 1));
    if (id == 7u) { buffer[1] = AMTPTP_MAX_PTP_CONTACTS; buffer[2] = 0u; }
    if (id == 8u) for (index = 0u; index < sizeof(hqa); ++index) buffer[index + 1u] = hqa[index];
    *written = needed;
    return AMTPTP_SOURCE_REPORT;
}

int amtptp_source_set_feature(amtptp_source *source, amtptp_u8 id,
    const amtptp_u8 *buffer, amtptp_size length)
{
    if (source == NULL || buffer == NULL || length < 2u || buffer[0] != id) return AMTPTP_SOURCE_INVALID;
    if (id == 4u) {
        if (buffer[1] != AMTPTP_MODE_MOUSE && buffer[1] != AMTPTP_MODE_TOUCHPAD) return AMTPTP_SOURCE_INVALID;
        if (source->input_mode != buffer[1]) source->release_pending = 1u;
        source->input_mode = buffer[1];
    } else if (id == 6u) {
        if ((buffer[1] & 0xfcu) != 0u) return AMTPTP_SOURCE_INVALID;
        if (source->button_enabled != (buffer[1] & 1u) ||
            source->surface_enabled != ((buffer[1] >> 1) & 1u)) source->release_pending = 1u;
        source->button_enabled = buffer[1] & 1u;
        source->surface_enabled = (buffer[1] >> 1) & 1u;
    } else return AMTPTP_SOURCE_INVALID;
    return AMTPTP_SOURCE_REPORT;
}

void amtptp_source_release(amtptp_source *source, amtptp_report_batch *batch)
{
    amtptp_u8 index;
    amtptp_frame release = source->previous;
    batch->count = 0u;
    release.button = 0u;
    for (index = 0u; index < release.contact_count; ++index) release.contacts[index].tip_switch = 0u;
    if (release.contact_count != 0u) append_frame(batch, &release);
    if (release.contact_count != 0u || source->previous.button != 0u) {
        release.contact_count = 0u;
        for (index = 0u; index < AMTPTP_MAX_PTP_CONTACTS; ++index) {
            amtptp_contact empty = {0};
            release.contacts[index] = empty;
        }
        append_frame(batch, &release);
    }
    clear_frame(&source->previous);
    amtptp_reset_session(&source->session);
    source->release_pending = 0u;
}

int amtptp_source_handshake(const amtptp_u8 *sdu, amtptp_size length)
{
    /* Linux hid-magicmouse documents an invalid-report-ID reply even when
     * Apple enables multitouch. Permit that exact quirk, then require an
     * independently validated 0x31 input before reporting ModeEnabled. */
    return sdu != NULL && length == 1u && (sdu[0] == 0u || sdu[0] == 2u);
}

int amtptp_source_input(amtptp_source *source, const amtptp_u8 *sdu,
    amtptp_size length, amtptp_report_batch *batch)
{
    amtptp_raw_frame raw;
    amtptp_frame frame, transition;
    amtptp_u8 old_index, new_index, missing = 0u;
    amtptp_u16 ids = 0u;
    if (source == NULL || sdu == NULL || batch == NULL) return AMTPTP_SOURCE_INVALID;
    batch->count = 0u;
    if (length < 2u || sdu[0] != AMTPTP_HIDP_INPUT) return AMTPTP_SOURCE_INVALID;
    if (sdu[1] != AMTPTP_APPLE_REPORT_ID) return AMTPTP_SOURCE_IGNORED;
    if (amtptp_decode_mt2(sdu + 1u, length - 1u, &raw) != AMTPTP_OK) return AMTPTP_SOURCE_INVALID;
    for (new_index = 0u; new_index < raw.contact_count; ++new_index) {
        amtptp_u16 bit = (amtptp_u16)(1u << raw.contacts[new_index].id);
        if ((ids & bit) != 0u) return AMTPTP_SOURCE_INVALID;
        ids |= bit;
    }
    if (source->input_mode != AMTPTP_MODE_TOUCHPAD || source->release_pending) return AMTPTP_SOURCE_IGNORED;
    if (amtptp_convert_ptp(&source->session, &source->options, &raw, &frame) != AMTPTP_OK) return AMTPTP_SOURCE_INVALID;
    if (!source->surface_enabled) {
        amtptp_frame empty = {0};
        empty.scan_time = frame.scan_time;
        empty.button = frame.button;
        frame = empty;
    }
    if (!source->button_enabled) frame.button = 0u;
    transition = source->previous;
    transition.scan_time = frame.scan_time;
    transition.button = frame.button;
    for (old_index = 0u; old_index < transition.contact_count; ++old_index) {
        for (new_index = 0u; new_index < frame.contact_count; ++new_index) {
            if (transition.contacts[old_index].id == frame.contacts[new_index].id) break;
        }
        if (new_index == frame.contact_count) {
            if (transition.contacts[old_index].tip_switch) missing = 1u;
            transition.contacts[old_index].tip_switch = 0u;
        } else transition.contacts[old_index] = frame.contacts[new_index];
    }
    if (missing) append_frame(batch, &transition);
    append_frame(batch, &frame);
    source->previous = frame;
    return AMTPTP_SOURCE_REPORT;
}
