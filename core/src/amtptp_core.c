#include "amtptp_core.h"

#define AMTPTP_UNUSED_CONTACT_ID 0xffffffffu

static uint16_t amtptp_read_u16_le(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t amtptp_read_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0]
        | ((uint32_t)input[1] << 8)
        | ((uint32_t)input[2] << 16)
        | ((uint32_t)input[3] << 24);
}

static void amtptp_write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & 0xffu);
    output[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void amtptp_write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value & 0xffu);
    output[1] = (uint8_t)((value >> 8) & 0xffu);
    output[2] = (uint8_t)((value >> 16) & 0xffu);
    output[3] = (uint8_t)((value >> 24) & 0xffu);
}

static int16_t amtptp_sign_extend_13(uint16_t value)
{
    value &= 0x1fffu;
    if ((value & 0x1000u) != 0u) {
        value |= 0xe000u;
    }
    return (int16_t)value;
}

static uint16_t amtptp_clamp_coordinate(int32_t value, uint16_t maximum)
{
    if (value <= 0) {
        return 0u;
    }
    if ((uint32_t)value >= (uint32_t)maximum) {
        return maximum;
    }
    return (uint16_t)value;
}

static uint16_t amtptp_active_id_mask(const amtptp_raw_frame *input)
{
    uint16_t mask = 0u;
    uint8_t index;

    for (index = 0u; index < input->contact_count; ++index) {
        mask |= (uint16_t)(1u << (input->contacts[index].id & 0x0fu));
    }
    return mask;
}

static amtptp_locked_contact *amtptp_find_lock(
    amtptp_session *session,
    uint32_t id)
{
    uint8_t index;

    for (index = 0u; index < 2u; ++index) {
        if (session->locked_contacts[index].id == id) {
            return &session->locked_contacts[index];
        }
    }
    return NULL;
}

static amtptp_locked_contact *amtptp_claim_lock(
    amtptp_session *session,
    uint32_t id)
{
    amtptp_locked_contact *existing = amtptp_find_lock(session, id);
    uint8_t index;

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
    uint8_t index;

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
    const uint8_t *input,
    size_t input_length,
    amtptp_raw_frame *output)
{
    size_t contact_count;
    size_t index;

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

    output->button = (uint8_t)(input[1] & 0x01u);
    output->timestamp_ms = ((uint32_t)amtptp_read_u16_le(&input[2]) << 5)
        | ((uint32_t)input[1] >> 3);
    output->contact_count = (uint8_t)contact_count;

    for (index = 0u; index < contact_count; ++index) {
        const uint8_t *encoded = &input[
            AMTPTP_APPLE_HEADER_SIZE + index * AMTPTP_APPLE_CONTACT_SIZE];
        uint32_t packed = amtptp_read_u32_le(encoded);
        amtptp_raw_contact *contact = &output->contacts[index];

        contact->absolute_x = amtptp_sign_extend_13(
            (uint16_t)(packed & 0x1fffu));
        contact->absolute_y = amtptp_sign_extend_13(
            (uint16_t)((packed >> 13) & 0x1fffu));
        contact->finger = (uint8_t)((packed >> 26) & 0x07u);
        contact->state = (uint8_t)((packed >> 29) & 0x07u);
        contact->touch_major = encoded[4];
        contact->touch_minor = encoded[5];
        contact->size = encoded[6];
        contact->pressure = encoded[7];
        contact->id = (uint8_t)(encoded[8] & 0x0fu);
        contact->orientation = (uint8_t)((encoded[8] >> 5) & 0x07u);
    }

    return AMTPTP_OK;
}

amtptp_status amtptp_convert_ptp(
    amtptp_session *session,
    const amtptp_options *options,
    const amtptp_raw_frame *input,
    amtptp_frame *output)
{
    uint16_t present_mask;
    uint16_t next_admitted_mask = 0u;
    uint8_t selected_indices[AMTPTP_MAX_PTP_CONTACTS];
    uint8_t admitted_count = 0u;
    uint8_t index;

    if (session == NULL || options == NULL || input == NULL || output == NULL) {
        return AMTPTP_ERROR_ARGUMENT;
    }
    if (input->contact_count > AMTPTP_MAX_RAW_CONTACTS) {
        return AMTPTP_ERROR_REPORT_LENGTH;
    }

    present_mask = amtptp_active_id_mask(input);
    session->suppressed_mask &= present_mask;
    session->admitted_mask &= present_mask;

    output->scan_time = (uint16_t)(input->timestamp_ms * 10u);
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
        uint16_t id_bit = (uint16_t)(1u << (raw->id & 0x0fu));

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
        uint16_t id_bit = (uint16_t)(1u << (raw->id & 0x0fu));

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
            int32_t x = (int32_t)raw->absolute_x - options->x_min;
            int32_t y = -(int32_t)raw->absolute_y - options->y_min;
            amtptp_contact *ptp = &output->contacts[index];
            amtptp_locked_contact *locked;
            uint8_t tip = (uint8_t)(
                (raw->state & 0x04u) != 0u
                && (options->ignore_near_fingers == 0u
                    || (raw->state & 0x02u) == 0u));
            uint8_t should_move = (uint8_t)(
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
            ptp->confidence = (uint8_t)(
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
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t index;
    size_t offset = 1u;

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
        output[offset] = (uint8_t)(
            (contact->confidence & 0x01u)
            | ((contact->tip_switch & 0x01u) << 1));
        amtptp_write_u32_le(&output[offset + 1u], contact->id);
        amtptp_write_u16_le(&output[offset + 5u], contact->x);
        amtptp_write_u16_le(&output[offset + 7u], contact->y);
        offset += AMTPTP_PTP_CONTACT_SIZE;
    }
    amtptp_write_u16_le(&output[offset], input->scan_time);
    output[offset + 2u] = input->contact_count;
    output[offset + 3u] = (uint8_t)(input->button & 0x01u);
    *output_length = AMTPTP_PTP_REPORT_SIZE;
    return AMTPTP_OK;
}
