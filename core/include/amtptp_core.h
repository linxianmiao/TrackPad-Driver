#ifndef AMTPTP_CORE_H
#define AMTPTP_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMTPTP_APPLE_REPORT_ID 0x31u
#define AMTPTP_PTP_REPORT_ID 0x05u
#define AMTPTP_MAX_RAW_CONTACTS 16u
#define AMTPTP_MAX_PTP_CONTACTS 5u
#define AMTPTP_APPLE_HEADER_SIZE 4u
#define AMTPTP_APPLE_CONTACT_SIZE 9u
#define AMTPTP_PTP_CONTACT_SIZE 9u
#define AMTPTP_PTP_REPORT_SIZE 50u

typedef enum amtptp_status {
    AMTPTP_OK = 0,
    AMTPTP_ERROR_ARGUMENT = -1,
    AMTPTP_ERROR_REPORT_ID = -2,
    AMTPTP_ERROR_REPORT_LENGTH = -3,
    AMTPTP_ERROR_OUTPUT_CAPACITY = -4
} amtptp_status;

typedef struct amtptp_raw_contact {
    int16_t absolute_x;
    int16_t absolute_y;
    uint8_t finger;
    uint8_t state;
    uint8_t touch_major;
    uint8_t touch_minor;
    uint8_t size;
    uint8_t pressure;
    uint8_t id;
    uint8_t orientation;
} amtptp_raw_contact;

typedef struct amtptp_raw_frame {
    uint32_t timestamp_ms;
    uint8_t button;
    uint8_t contact_count;
    amtptp_raw_contact contacts[AMTPTP_MAX_RAW_CONTACTS];
} amtptp_raw_frame;

typedef struct amtptp_contact {
    uint32_t id;
    uint16_t x;
    uint16_t y;
    uint8_t confidence;
    uint8_t tip_switch;
} amtptp_contact;

typedef struct amtptp_frame {
    uint16_t scan_time;
    uint8_t button;
    uint8_t contact_count;
    amtptp_contact contacts[AMTPTP_MAX_PTP_CONTACTS];
} amtptp_frame;

typedef struct amtptp_options {
    int16_t x_min;
    int16_t y_min;
    uint16_t x_max;
    uint16_t y_max;
    uint32_t stop_pressure;
    uint32_t stop_size;
    uint8_t button_disabled;
    uint8_t ignore_button_finger;
    uint8_t ignore_near_fingers;
    uint8_t palm_rejection;
} amtptp_options;

typedef struct amtptp_locked_contact {
    uint32_t id;
    uint16_t x;
    uint16_t y;
    uint8_t tip_switch;
    uint8_t locked;
} amtptp_locked_contact;

typedef struct amtptp_session {
    uint16_t admitted_mask;
    uint16_t suppressed_mask;
    uint8_t previous_button;
    amtptp_locked_contact locked_contacts[2];
} amtptp_session;

void amtptp_default_options(amtptp_options *options);
void amtptp_reset_session(amtptp_session *session);

amtptp_status amtptp_decode_mt2(
    const uint8_t *input,
    size_t input_length,
    amtptp_raw_frame *output);

amtptp_status amtptp_convert_ptp(
    amtptp_session *session,
    const amtptp_options *options,
    const amtptp_raw_frame *input,
    amtptp_frame *output);

amtptp_status amtptp_serialize_ptp(
    const amtptp_frame *input,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif
