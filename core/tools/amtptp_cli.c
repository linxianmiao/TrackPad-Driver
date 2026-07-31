#include "amtptp_core.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_LINE_CAPACITY 8192u
#define CLI_REPORT_CAPACITY \
    (AMTPTP_APPLE_HEADER_SIZE \
        + AMTPTP_MAX_RAW_CONTACTS * AMTPTP_APPLE_CONTACT_SIZE)

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int extract_string(
    const char *line,
    const char *key,
    char *output,
    size_t capacity)
{
    char pattern[64];
    const char *start;
    const char *end;
    size_t length;

    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", key) < 0) {
        return 0;
    }
    start = strstr(line, pattern);
    if (start == NULL) {
        return 0;
    }
    start += strlen(pattern);
    end = strchr(start, '"');
    if (end == NULL) {
        return 0;
    }
    length = (size_t)(end - start);
    if (length + 1u > capacity) {
        return 0;
    }
    memcpy(output, start, length);
    output[length] = '\0';
    return 1;
}

static int parse_hex(
    const char *hex,
    uint8_t *output,
    size_t capacity,
    size_t *output_length)
{
    size_t hex_length = strlen(hex);
    size_t index;

    if ((hex_length & 1u) != 0u || hex_length / 2u > capacity) {
        return 0;
    }

    for (index = 0u; index < hex_length; index += 2u) {
        int high = hex_value(hex[index]);
        int low = hex_value(hex[index + 1u]);
        if (high < 0 || low < 0) {
            return 0;
        }
        output[index / 2u] = (uint8_t)((high << 4) | low);
    }
    *output_length = hex_length / 2u;
    return 1;
}

static void print_hex(const uint8_t *input, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0u; index < length; ++index) {
        putchar(digits[(input[index] >> 4) & 0x0fu]);
        putchar(digits[input[index] & 0x0fu]);
    }
}

static void print_error(const char *request_id, int status, const char *message)
{
    printf(
        "{\"requestId\":\"%s\",\"ok\":false,\"status\":%d,"
        "\"error\":\"%s\"}\n",
        request_id,
        status,
        message);
}

static void print_result(
    const char *request_id,
    const amtptp_raw_frame *raw,
    const amtptp_frame *ptp,
    const uint8_t *ptp_bytes,
    size_t ptp_length)
{
    uint8_t index;

    printf(
        "{\"requestId\":\"%s\",\"ok\":true,"
        "\"decoded\":{\"timestampMs\":%u,\"button\":%u,\"contacts\":[",
        request_id,
        (unsigned int)raw->timestamp_ms,
        (unsigned int)raw->button);
    for (index = 0u; index < raw->contact_count; ++index) {
        const amtptp_raw_contact *contact = &raw->contacts[index];
        if (index != 0u) {
            putchar(',');
        }
        printf(
            "{\"id\":%u,\"absoluteX\":%d,\"absoluteY\":%d,"
            "\"finger\":%u,\"state\":%u,\"touchMajor\":%u,"
            "\"touchMinor\":%u,\"size\":%u,\"pressure\":%u,"
            "\"orientation\":%u}",
            (unsigned int)contact->id,
            (int)contact->absolute_x,
            (int)contact->absolute_y,
            (unsigned int)contact->finger,
            (unsigned int)contact->state,
            (unsigned int)contact->touch_major,
            (unsigned int)contact->touch_minor,
            (unsigned int)contact->size,
            (unsigned int)contact->pressure,
            (unsigned int)contact->orientation);
    }
    printf(
        "]},\"ptp\":{\"scanTime\":%u,\"button\":%u,\"contactCount\":%u,"
        "\"contacts\":[",
        (unsigned int)ptp->scan_time,
        (unsigned int)ptp->button,
        (unsigned int)ptp->contact_count);
    for (index = 0u; index < ptp->contact_count; ++index) {
        const amtptp_contact *contact = &ptp->contacts[index];
        if (index != 0u) {
            putchar(',');
        }
        printf(
            "{\"id\":%u,\"x\":%u,\"y\":%u,\"confidence\":%u,"
            "\"tipSwitch\":%u}",
            (unsigned int)contact->id,
            (unsigned int)contact->x,
            (unsigned int)contact->y,
            (unsigned int)contact->confidence,
            (unsigned int)contact->tip_switch);
    }
    printf("]},\"ptpReportHex\":\"");
    print_hex(ptp_bytes, ptp_length);
    printf("\"}\n");
}

int main(void)
{
    char line[CLI_LINE_CAPACITY];
    char request_id[128];
    char report_hex[CLI_REPORT_CAPACITY * 2u + 1u];
    uint8_t raw_bytes[CLI_REPORT_CAPACITY];
    uint8_t ptp_bytes[AMTPTP_PTP_REPORT_SIZE];
    amtptp_raw_frame raw;
    amtptp_frame ptp;
    amtptp_options options;
    amtptp_session session;

    amtptp_default_options(&options);
    amtptp_reset_session(&session);
    setvbuf(stdout, NULL, _IOLBF, 0);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t raw_length = 0u;
        size_t ptp_length = 0u;
        amtptp_status status;

        if (!extract_string(
                line, "requestId", request_id, sizeof(request_id))) {
            strcpy(request_id, "unknown");
        }

        if (strstr(line, "\"command\":\"reset\"") != NULL) {
            amtptp_reset_session(&session);
            printf(
                "{\"requestId\":\"%s\",\"ok\":true,\"reset\":true}\n",
                request_id);
            continue;
        }

        if (!extract_string(
                line, "reportHex", report_hex, sizeof(report_hex))) {
            print_error(request_id, AMTPTP_ERROR_ARGUMENT, "missing reportHex");
            continue;
        }
        if (!parse_hex(
                report_hex,
                raw_bytes,
                sizeof(raw_bytes),
                &raw_length)) {
            print_error(request_id, AMTPTP_ERROR_ARGUMENT, "invalid reportHex");
            continue;
        }

        status = amtptp_decode_mt2(raw_bytes, raw_length, &raw);
        if (status != AMTPTP_OK) {
            print_error(request_id, status, "Apple report decode failed");
            continue;
        }
        status = amtptp_convert_ptp(&session, &options, &raw, &ptp);
        if (status != AMTPTP_OK) {
            print_error(request_id, status, "PTP conversion failed");
            continue;
        }
        status = amtptp_serialize_ptp(
            &ptp, ptp_bytes, sizeof(ptp_bytes), &ptp_length);
        if (status != AMTPTP_OK) {
            print_error(request_id, status, "PTP serialization failed");
            continue;
        }

        print_result(request_id, &raw, &ptp, ptp_bytes, ptp_length);
    }
    return 0;
}
