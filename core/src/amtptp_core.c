#include "amtptp_core.h"

#define AMTPTP_UNUSED_CONTACT_ID 0xffffffffu

static amtptp_u16 amtptp_read_u16_le(const amtptp_u8 *input)
{
    return (amtptp_u16)((amtptp_u16)input[0] | ((amtptp_u16)input[1] << 8));
}

static amtptp_u32 amtptp_read_u32_le(const amtptp_u8 *input)
{
    return (amtptp_u32)input[0]
        | ((amtptp_u32)input[1] << 8)
        | ((amtptp_u32)input[2] << 16)
        | ((amtptp_u32)input[3] << 24);
}

static void amtptp_write_u16_le(amtptp_u8 *output, amtptp_u16 value)
{
    output[0] = (amtptp_u8)(value & 0xffu);
    output[1] = (amtptp_u8)((value >> 8) & 0xffu);
}

static void amtptp_write_u32_le(amtptp_u8 *output, amtptp_u32 value)
{
    output[0] = (amtptp_u8)(value & 0xffu);
    output[1] = (amtptp_u8)((value >> 8) & 0xffu);
    output[2] = (amtptp_u8)((value >> 16) & 0xffu);
    output[3] = (amtptp_u8)((value >> 24) & 0xffu);
}

static amtptp_i16 amtptp_sign_extend_13(amtptp_u16 value)
{
    value &= 0x1fffu;
    if ((value & 0x1000u) != 0u) {
        value |= 0xe000u;
    }
    return (amtptp_i16)value;
}

static amtptp_u16 amtptp_clamp_coordinate(amtptp_i32 value, amtptp_u16 maximum)
{
    if (value <= 0) {
        return 0u;
    }
    if ((amtptp_u32)value >= (amtptp_u32)maximum) {
        return maximum;
    }
    return (amtptp_u16)value;
}

static amtptp_u16 amtptp_active_id_mask(const amtptp_raw_frame *input)
{
    amtptp_u16 mask = 0u;
    amtptp_u8 index;

    for (index = 0u; index < input->contact_count; ++index) {
        mask |= (amtptp_u16)(1u << (input->contacts[index].id & 0x0fu));
    }
    return mask;
}

static amtptp_locked_contact *amtptp_find_lock(
    amtptp_session *session,
    amtptp_u32 id)
{
    amtptp_u8 index;

    for (index = 0u; index < 2u; ++index) {
        if (session->locked_contacts[index].id == id) {
            return &session->locked_contacts[index];
        }
    }
    return NULL;
}

static amtptp_locked_contact *amtptp_claim_lock(
    amtptp_session *session,
    amtptp_u32 id)
{
    amtptp_locked_contact *existing = amtptp_find_lock(session, id);
    amtptp_u8 index;

    if (existing != NULL) {
        return existing;
    }

    for (index = 0u; index < 2u; ++index) {
        if (session->locked_contacts[index].tip_switch == 0u) {
            session->locked_contacts[index].id = id;
            session->locked_contacts[index].locked = 0u;
            return &session->locked_contacts[index];
        }
    }

    session->locked_contacts[0].id = id;
    session->locked_contacts[0].locked = 0u;
    return &session->locked_contacts[0];
}

void amtptp_default_options(amtptp_options *options)
{
    if (options == NULL) {
        return;
    }

    options->x_min = -3678;
    options->y_min = -2479;
    options->x_max = 7612u;
    options->y_max = 5065u;
    options->stop_pressure = 0xffffffffu;
    options->stop_size = 0xffffffffu;
    options->button_disabled = 0u;
    options->ignore_button_finger = 0u;
    options->ignore_near_fingers = 1u;
    options->palm_rejection = 1u;
}

void amtptp_reset_session(amtptp_session *session)
{
    amtptp_u8 index;

    if (session == NULL) {
        return;
    }

    session->admitted_mask = 0u;
    session->suppressed_mask = 0u;
    session->previous_button = 0u;

    for (index = 0u; index < 2u; ++index) {
        session->locked_contacts[index].id = AMTPTP_UNUSED_CONTACT_ID;
        session->locked_contacts[index].x = 0u;
        session->locked_contacts[index].y = 0u;
        session->locked_contacts[index].tip_switch = 0u;
        session->locked_contacts[index].locked = 0u;
    }
}

amtptp_status amtptp_decode_mt2(
    const amtptp_u8 *input,
    amtptp_size input_length,
    amtptp_raw_frame *output)
{
    amtptp_size contact_count;
    amtptp_size index;

    if (input == NULL || output == NULL) {
        return AMTPTP_ERROR_ARGUMENT;
    }
    if (input_length < AMTPTP_APPLE_HEADER_SIZE) {
        return AMTPTP_ERROR_REPORT_LENGTH;
    }
    if (input[0] != AMTPTP_APPLE_REPORT_ID) {
        return AMTPTP_ERROR_REPORT_ID;
    }
    if ((input_length - AMTPTP_APPLE_HEADER_SIZE)
        % AMTPTP_APPLE_CONTACT_SIZE != 0u) {
        return AMTPTP_ERROR_REPORT_LENGTH;
    }

    contact_count = (input_length - AMTPTP_APPLE_HEADER_SIZE)
        / AMTPTP_APPLE_CONTACT_SIZE;
    if (contact_count > AMTPTP_MAX_RAW_CONTACTS) {
        return AMTPTP_ERROR_REPORT_LENGTH;
    }

    output->button = (amtptp_u8)(input[1] & 0x01u);
    output->timestamp_ms = ((amtptp_u32)amtptp_read_u16_le(&input[2]) << 5)
        | ((amtptp_u32)input[1] >> 3);
    output->contact_count = (amtptp_u8)contact_count;

    for (index = 0u; index < contact_count; ++index) {
        const amtptp_u8 *encoded = &input[
            AMTPTP_APPLE_HEADER_SIZE + index * AMTPTP_APPLE_CONTACT_SIZE];
        amtptp_u32 packed = amtptp_read_u32_le(encoded);
        amtptp_raw_contact *contact = &output->contacts[index];

        contact->absolute_x = amtptp_sign_extend_13(
            (amtptp_u16)(packed & 0x1fffu));
        contact->absolute_y = amtptp_sign_extend_13(
            (amtptp_u16)((packed >> 13) & 0x1fffu));
        contact->finger = (amtptp_u8)((packed >> 26) & 0x07u);
        contact->state = (amtptp_u8)((packed >> 29) & 0x07u);
        contact->touch_major = encoded[4];
        contact->touch_minor = encoded[5];
        contact->size = encoded[6];
        contact->pressure = encoded[7];
        contact->id = (amtptp_u8)(encoded[8] & 0x0fu);
        contact->orientation = (amtptp_u8)((encoded[8] >> 5) & 0x07u);
    }

    return AMTPTP_OK;
}

amtptp_status amtptp_convert_ptp(
    amtptp_session *session,
    const amtptp_options *options,
    const amtptp_raw_frame *input,
    amtptp_frame *output)
{
    amtptp_u16 present_mask;
    amtptp_u16 next_admitted_mask = 0u;
    amtptp_u8 selected_indices[AMTPTP_MAX_PTP_CONTACTS];
    amtptp_u8 admitted_count = 0u;
    amtptp_u8 index;

    if (session == NULL || options == NULL || input == NULL || output == NULL) {
        return AMTPTP_ERROR_ARGUMENT;
    }
    if (input->contact_count > AMTPTP_MAX_RAW_CONTACTS) {
        return AMTPTP_ERROR_REPORT_LENGTH;
    }

    present_mask = amtptp_active_id_mask(input);
    session->suppressed_mask &= present_mask;
    session->admitted_mask &= present_mask;

    output->scan_time = (amtptp_u16)(input->timestamp_ms * 10u);
    output->button = options->button_disabled != 0u ? 0u : input->button;
    output->contact_count = 0u;

    for (index = 0u; index < AMTPTP_MAX_PTP_CONTACTS; ++index) {
        output->contacts[index].id = 0u;
        output->contacts[index].x = 0u;
        output->contacts[index].y = 0u;
        output->contacts[index].confidence = 0u;
        output->contacts[index].tip_switch = 0u;
    }

    /*
     * Preserve every still-present contact that was admitted in the previous
     * frame before considering new IDs. Apple is free to reorder contacts
     * between frames; allowing report order to decide admission would make a
     * sixth finger evict an existing PTP contact without a lift transition.
     */
    for (index = 0u; index < input->contact_count; ++index) {
        const amtptp_raw_contact *raw = &input->contacts[index];
        amtptp_u16 id_bit = (amtptp_u16)(1u << (raw->id & 0x0fu));

        if ((session->admitted_mask & id_bit) == 0u
            || (session->suppressed_mask & id_bit) != 0u
            || (next_admitted_mask & id_bit) != 0u) {
            continue;
        }

        selected_indices[admitted_count] = index;
        next_admitted_mask |= id_bit;
        ++admitted_count;
    }

    for (index = 0u; index < input->contact_count; ++index) {
        const amtptp_raw_contact *raw = &input->contacts[index];
        amtptp_u16 id_bit = (amtptp_u16)(1u << (raw->id & 0x0fu));

        if ((session->admitted_mask & id_bit) != 0u
            || (session->suppressed_mask & id_bit) != 0u
            || (next_admitted_mask & id_bit) != 0u) {
            continue;
        }
        if (admitted_count >= AMTPTP_MAX_PTP_CONTACTS) {
            session->suppressed_mask |= id_bit;
            continue;
        }

        selected_indices[admitted_count] = index;
        next_admitted_mask |= id_bit;
        ++admitted_count;
    }

    for (index = 0u; index < admitted_count; ++index) {
        const amtptp_raw_contact *raw =
            &input->contacts[selected_indices[index]];
        {
            amtptp_i32 x = (amtptp_i32)raw->absolute_x - options->x_min;
            amtptp_i32 y = -(amtptp_i32)raw->absolute_y - options->y_min;
            amtptp_contact *ptp = &output->contacts[index];
            amtptp_locked_contact *locked;
            amtptp_u8 tip = (amtptp_u8)(
                (raw->state & 0x04u) != 0u
                && (options->ignore_near_fingers == 0u
                    || (raw->state & 0x02u) == 0u));
            amtptp_u8 should_move = (amtptp_u8)(
                (options->ignore_button_finger == 0u
                    || session->previous_button == 0u
                    || output->button == 0u)
                && (options->stop_pressure == 0xffffffffu
                    || raw->pressure > options->stop_pressure)
                && (options->stop_size == 0xffffffffu
                    || raw->size > options->stop_size));

            ptp->id = raw->id;
            ptp->x = amtptp_clamp_coordinate(x, options->x_max);
            ptp->y = amtptp_clamp_coordinate(y, options->y_max);
            ptp->tip_switch = tip;
            ptp->confidence = (amtptp_u8)(
                options->palm_rejection == 0u || raw->finger != 6u);

            locked = amtptp_claim_lock(session, raw->id);
            if (should_move != 0u && locked->locked == 0u) {
                locked->x = ptp->x;
                locked->y = ptp->y;
                locked->tip_switch = tip;
            } else if (tip != 0u) {
                locked->tip_switch = 1u;
                locked->locked = 1u;
                ptp->x = locked->x;
                ptp->y = locked->y;
            } else {
                locked->id = AMTPTP_UNUSED_CONTACT_ID;
                locked->tip_switch = 0u;
                locked->locked = 0u;
            }
        }
    }

    output->contact_count = admitted_count;
    session->admitted_mask = next_admitted_mask;
    session->previous_button = output->button;
    return AMTPTP_OK;
}

amtptp_status amtptp_serialize_ptp(
    const amtptp_frame *input,
    amtptp_u8 *output,
    amtptp_size output_capacity,
    amtptp_size *output_length)
{
    amtptp_size index;
    amtptp_size offset = 1u;

    if (input == NULL || output == NULL || output_length == NULL) {
        return AMTPTP_ERROR_ARGUMENT;
    }
    if (output_capacity < AMTPTP_PTP_REPORT_SIZE) {
        return AMTPTP_ERROR_OUTPUT_CAPACITY;
    }
    if (input->contact_count > AMTPTP_MAX_PTP_CONTACTS) {
        return AMTPTP_ERROR_REPORT_LENGTH;
    }

    output[0] = AMTPTP_PTP_REPORT_ID;
    for (index = 0u; index < AMTPTP_MAX_PTP_CONTACTS; ++index) {
        const amtptp_contact *contact = &input->contacts[index];
        output[offset] = (amtptp_u8)(
            (contact->confidence & 0x01u)
            | ((contact->tip_switch & 0x01u) << 1));
        amtptp_write_u32_le(&output[offset + 1u], contact->id);
        amtptp_write_u16_le(&output[offset + 5u], contact->x);
        amtptp_write_u16_le(&output[offset + 7u], contact->y);
        offset += AMTPTP_PTP_CONTACT_SIZE;
    }
    amtptp_write_u16_le(&output[offset], input->scan_time);
    output[offset + 2u] = input->contact_count;
    output[offset + 3u] = (amtptp_u8)(input->button & 0x01u);
    *output_length = AMTPTP_PTP_REPORT_SIZE;
    return AMTPTP_OK;
}
