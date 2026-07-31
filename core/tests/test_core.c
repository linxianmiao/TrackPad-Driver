#include "amtptp_core.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures; \
        } \
    } while (0)

static uint16_t encode_signed_13(int16_t value)
{
    return (uint16_t)value & 0x1fffu;
}

static size_t encode_frame(
    uint8_t *output,
    uint32_t timestamp_ms,
    uint8_t button,
    const amtptp_raw_contact *contacts,
    size_t contact_count)
{
    size_t index;

    output[0] = AMTPTP_APPLE_REPORT_ID;
    output[1] = (uint8_t)(
        (button & 0x01u) | ((timestamp_ms & 0x1fu) << 3));
    output[2] = (uint8_t)((timestamp_ms >> 5) & 0xffu);
    output[3] = (uint8_t)((timestamp_ms >> 13) & 0xffu);

    for (index = 0u; index < contact_count; ++index) {
        const amtptp_raw_contact *contact = &contacts[index];
        uint32_t packed =
            (uint32_t)encode_signed_13(contact->absolute_x)
            | ((uint32_t)encode_signed_13(contact->absolute_y) << 13)
            | ((uint32_t)(contact->finger & 0x07u) << 26)
            | ((uint32_t)(contact->state & 0x07u) << 29);
        size_t offset = AMTPTP_APPLE_HEADER_SIZE
            + index * AMTPTP_APPLE_CONTACT_SIZE;

        output[offset] = (uint8_t)(packed & 0xffu);
        output[offset + 1u] = (uint8_t)((packed >> 8) & 0xffu);
        output[offset + 2u] = (uint8_t)((packed >> 16) & 0xffu);
        output[offset + 3u] = (uint8_t)((packed >> 24) & 0xffu);
        output[offset + 4u] = contact->touch_major;
        output[offset + 5u] = contact->touch_minor;
        output[offset + 6u] = contact->size;
        output[offset + 7u] = contact->pressure;
        output[offset + 8u] = (uint8_t)(
            (contact->id & 0x0fu)
            | ((contact->orientation & 0x07u) << 5));
    }

    return AMTPTP_APPLE_HEADER_SIZE
        + contact_count * AMTPTP_APPLE_CONTACT_SIZE;
}

static void test_single_contact(void)
{
    uint8_t raw[AMTPTP_APPLE_HEADER_SIZE + AMTPTP_APPLE_CONTACT_SIZE];
    uint8_t serialized[AMTPTP_PTP_REPORT_SIZE];
    amtptp_raw_contact source = {
        -1000, 500, 2u, 4u, 12u, 10u, 14u, 30u, 3u, 5u
    };
    amtptp_raw_frame decoded;
    amtptp_frame ptp;
    amtptp_options options;
    amtptp_session session;
    size_t raw_length;
    size_t output_length = 0u;

    amtptp_default_options(&options);
    amtptp_reset_session(&session);
    raw_length = encode_frame(raw, 1234u, 1u, &source, 1u);

    CHECK(
        amtptp_decode_mt2(raw, raw_length, &decoded) == AMTPTP_OK,
        "single contact report decodes");
    CHECK(decoded.timestamp_ms == 1234u, "timestamp round-trips");
    CHECK(decoded.button == 1u, "button round-trips");
    CHECK(decoded.contacts[0].absolute_x == -1000, "x sign extends");
    CHECK(decoded.contacts[0].absolute_y == 500, "y sign extends");
    CHECK(decoded.contacts[0].id == 3u, "contact id round-trips");

    CHECK(
        amtptp_convert_ptp(&session, &options, &decoded, &ptp)
            == AMTPTP_OK,
        "single contact converts");
    CHECK(ptp.scan_time == 12340u, "scan time uses 100us units");
    CHECK(ptp.contact_count == 1u, "contact count is one");
    CHECK(ptp.button == 1u, "button is reported");
    CHECK(ptp.contacts[0].x == 2678u, "x is normalized");
    CHECK(ptp.contacts[0].y == 1979u, "y is normalized and inverted");
    CHECK(ptp.contacts[0].tip_switch == 1u, "tip switch is set");
    CHECK(ptp.contacts[0].confidence == 1u, "contact is confident");

    CHECK(
        amtptp_serialize_ptp(
            &ptp, serialized, sizeof(serialized), &output_length)
            == AMTPTP_OK,
        "PTP report serializes");
    CHECK(output_length == AMTPTP_PTP_REPORT_SIZE, "PTP report is 50 bytes");
    CHECK(serialized[0] == AMTPTP_PTP_REPORT_ID, "PTP report id is 5");
    CHECK(serialized[1] == 0x03u, "status byte packs confidence and tip");
    CHECK(serialized[2] == 3u, "contact id is little endian");
    CHECK(serialized[48] == 1u, "serialized contact count is one");
    CHECK(serialized[49] == 1u, "serialized button is set");
}

static void test_validation(void)
{
    uint8_t malformed[] = { AMTPTP_APPLE_REPORT_ID, 0u, 0u, 0u, 1u };
    uint8_t wrong_id[] = { 0x30u, 0u, 0u, 0u };
    amtptp_raw_frame output;

    CHECK(
        amtptp_decode_mt2(malformed, sizeof(malformed), &output)
            == AMTPTP_ERROR_REPORT_LENGTH,
        "malformed report length is rejected");
    CHECK(
        amtptp_decode_mt2(wrong_id, sizeof(wrong_id), &output)
            == AMTPTP_ERROR_REPORT_ID,
        "unexpected report id is rejected");
}

static void test_palm_and_near_finger(void)
{
    uint8_t raw[AMTPTP_APPLE_HEADER_SIZE + 2u * AMTPTP_APPLE_CONTACT_SIZE];
    amtptp_raw_contact contacts[2] = {
        { 0, 0, 6u, 4u, 10u, 10u, 10u, 20u, 1u, 0u },
        { 100, -100, 2u, 6u, 10u, 10u, 10u, 20u, 2u, 0u }
    };
    amtptp_raw_frame decoded;
    amtptp_frame ptp;
    amtptp_options options;
    amtptp_session session;
    size_t length;

    amtptp_default_options(&options);
    amtptp_reset_session(&session);
    length = encode_frame(raw, 10u, 0u, contacts, 2u);
    CHECK(
        amtptp_decode_mt2(raw, length, &decoded) == AMTPTP_OK,
        "palm report decodes");
    CHECK(
        amtptp_convert_ptp(&session, &options, &decoded, &ptp)
            == AMTPTP_OK,
        "palm report converts");
    CHECK(ptp.contacts[0].confidence == 0u, "palm loses confidence");
    CHECK(ptp.contacts[1].tip_switch == 0u, "near finger clears tip");
}

static void test_contact_limit_is_stable(void)
{
    uint8_t raw[
        AMTPTP_APPLE_HEADER_SIZE + 6u * AMTPTP_APPLE_CONTACT_SIZE];
    amtptp_raw_contact contacts[6];
    amtptp_raw_frame decoded;
    amtptp_frame ptp;
    amtptp_options options;
    amtptp_session session;
    size_t index;
    size_t length;

    for (index = 0u; index < 6u; ++index) {
        contacts[index].absolute_x = (int16_t)(index * 100);
        contacts[index].absolute_y = 0;
        contacts[index].finger = 2u;
        contacts[index].state = 4u;
        contacts[index].touch_major = 10u;
        contacts[index].touch_minor = 10u;
        contacts[index].size = 10u;
        contacts[index].pressure = 20u;
        contacts[index].id = (uint8_t)index;
        contacts[index].orientation = 0u;
    }

    amtptp_default_options(&options);
    amtptp_reset_session(&session);
    length = encode_frame(raw, 20u, 0u, contacts, 6u);
    CHECK(
        amtptp_decode_mt2(raw, length, &decoded) == AMTPTP_OK,
        "six contact report decodes");
    CHECK(
        amtptp_convert_ptp(&session, &options, &decoded, &ptp)
            == AMTPTP_OK,
        "six contact report converts");
    CHECK(ptp.contact_count == 5u, "PTP contact count is capped at five");
    CHECK((session.suppressed_mask & (1u << 5)) != 0u, "sixth id is suppressed");

    contacts[0].id = 5u;
    contacts[5].id = 0u;
    length = encode_frame(raw, 28u, 0u, contacts, 6u);
    CHECK(
        amtptp_decode_mt2(raw, length, &decoded) == AMTPTP_OK,
        "reordered contact report decodes");
    CHECK(
        amtptp_convert_ptp(&session, &options, &decoded, &ptp)
            == AMTPTP_OK,
        "reordered contact report converts");
    CHECK(ptp.contact_count == 5u, "reordering does not exceed five");
    for (index = 0u; index < ptp.contact_count; ++index) {
        CHECK(ptp.contacts[index].id != 5u, "suppressed id stays suppressed");
    }
}

static void test_new_contact_cannot_evict_reordered_contact(void)
{
    uint8_t raw[
        AMTPTP_APPLE_HEADER_SIZE + 6u * AMTPTP_APPLE_CONTACT_SIZE];
    amtptp_raw_contact contacts[6];
    amtptp_raw_frame decoded;
    amtptp_frame ptp;
    amtptp_options options;
    amtptp_session session;
    size_t index;
    size_t length;

    for (index = 0u; index < 6u; ++index) {
        contacts[index].absolute_x = (int16_t)(index * 100);
        contacts[index].absolute_y = 0;
        contacts[index].finger = 2u;
        contacts[index].state = 4u;
        contacts[index].touch_major = 10u;
        contacts[index].touch_minor = 10u;
        contacts[index].size = 10u;
        contacts[index].pressure = 20u;
        contacts[index].id = (uint8_t)index;
        contacts[index].orientation = 0u;
    }

    amtptp_default_options(&options);
    amtptp_reset_session(&session);
    length = encode_frame(raw, 30u, 0u, contacts, 5u);
    CHECK(
        amtptp_decode_mt2(raw, length, &decoded) == AMTPTP_OK,
        "five-contact baseline decodes");
    CHECK(
        amtptp_convert_ptp(&session, &options, &decoded, &ptp)
            == AMTPTP_OK,
        "five-contact baseline converts");

    contacts[0].id = 5u;
    contacts[1].id = 0u;
    contacts[2].id = 1u;
    contacts[3].id = 2u;
    contacts[4].id = 3u;
    contacts[5].id = 4u;
    length = encode_frame(raw, 38u, 0u, contacts, 6u);
    CHECK(
        amtptp_decode_mt2(raw, length, &decoded) == AMTPTP_OK,
        "new-first reordered report decodes");
    CHECK(
        amtptp_convert_ptp(&session, &options, &decoded, &ptp)
            == AMTPTP_OK,
        "new-first reordered report converts");
    CHECK(
        (session.suppressed_mask & (1u << 5)) != 0u,
        "new sixth contact is suppressed");
    for (index = 0u; index < ptp.contact_count; ++index) {
        CHECK(ptp.contacts[index].id != 5u, "new id cannot evict old id");
    }
}

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void test_randomized_report_invariants(void)
{
    uint8_t raw[
        AMTPTP_APPLE_HEADER_SIZE
        + AMTPTP_MAX_RAW_CONTACTS * AMTPTP_APPLE_CONTACT_SIZE];
    uint8_t serialized[AMTPTP_PTP_REPORT_SIZE];
    amtptp_raw_contact contacts[AMTPTP_MAX_RAW_CONTACTS];
    amtptp_raw_frame decoded;
    amtptp_frame ptp;
    amtptp_options options;
    amtptp_session session;
    uint32_t random_state = 0x0324a11eu;
    size_t iteration;

    amtptp_default_options(&options);
    amtptp_reset_session(&session);

    for (iteration = 0u; iteration < 2000u; ++iteration) {
        size_t contact_count =
            next_random(&random_state) % (AMTPTP_MAX_RAW_CONTACTS + 1u);
        size_t index;
        size_t length;
        size_t output_length = 0u;
        uint16_t output_ids = 0u;

        for (index = 0u; index < contact_count; ++index) {
            contacts[index].absolute_x = (int16_t)(
                (int32_t)(next_random(&random_state) % 8192u) - 4096);
            contacts[index].absolute_y = (int16_t)(
                (int32_t)(next_random(&random_state) % 8192u) - 4096);
            contacts[index].finger =
                (uint8_t)(next_random(&random_state) & 0x07u);
            contacts[index].state =
                (uint8_t)(next_random(&random_state) & 0x07u);
            contacts[index].touch_major =
                (uint8_t)next_random(&random_state);
            contacts[index].touch_minor =
                (uint8_t)next_random(&random_state);
            contacts[index].size =
                (uint8_t)next_random(&random_state);
            contacts[index].pressure =
                (uint8_t)next_random(&random_state);
            contacts[index].id =
                (uint8_t)(next_random(&random_state) & 0x0fu);
            contacts[index].orientation =
                (uint8_t)(next_random(&random_state) & 0x07u);
        }

        length = encode_frame(
            raw,
            next_random(&random_state) & 0x1fffffu,
            (uint8_t)(next_random(&random_state) & 1u),
            contacts,
            contact_count);
        CHECK(
            amtptp_decode_mt2(raw, length, &decoded) == AMTPTP_OK,
            "random report decodes");
        CHECK(
            amtptp_convert_ptp(&session, &options, &decoded, &ptp)
                == AMTPTP_OK,
            "random report converts");
        CHECK(
            ptp.contact_count <= AMTPTP_MAX_PTP_CONTACTS,
            "random report stays within the PTP contact cap");

        for (index = 0u; index < ptp.contact_count; ++index) {
            uint16_t id_bit =
                (uint16_t)(1u << (ptp.contacts[index].id & 0x0fu));
            CHECK(
                (output_ids & id_bit) == 0u,
                "random report emits unique contact ids");
            CHECK(
                ptp.contacts[index].x <= options.x_max,
                "random x is clamped");
            CHECK(
                ptp.contacts[index].y <= options.y_max,
                "random y is clamped");
            output_ids |= id_bit;
        }

        CHECK(
            amtptp_serialize_ptp(
                &ptp,
                serialized,
                sizeof(serialized),
                &output_length) == AMTPTP_OK,
            "random PTP frame serializes");
        CHECK(
            output_length == AMTPTP_PTP_REPORT_SIZE,
            "random PTP frame is exactly 50 bytes");
    }
}

int main(void)
{
    test_single_contact();
    test_validation();
    test_palm_and_near_finger();
    test_contact_limit_is_stable();
    test_new_contact_cannot_evict_reordered_contact();
    test_randomized_report_invariants();

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    printf("All amtptp core tests passed.\n");
    return 0;
}
