#ifndef AMTPTP_CORE_H
#define AMTPTP_CORE_H

#if defined(_KERNEL_MODE)
#include <ntdef.h>
typedef SIZE_T amtptp_size;
#else
#include <stddef.h>
#include <stdint.h>
typedef size_t amtptp_size;
#endif

/*
 * Keep the shared decoder independent from the user-mode CRT.  Including
 * <stdint.h> in a KMDF translation unit mixes the Visual C++ CRT headers with
 * the WDK kernel CRT headers.  The supported targets use 8-bit bytes, 16-bit
 * shorts and 32-bit ints; the compile-time checks below make that contract
 * explicit without exporting platform-specific Windows types.
 */
typedef signed short amtptp_i16;
typedef signed int amtptp_i32;
typedef unsigned char amtptp_u8;
typedef unsigned short amtptp_u16;
typedef unsigned int amtptp_u32;

typedef char amtptp_i16_must_be_2[(sizeof(amtptp_i16) == 2u) ? 1 : -1];
typedef char amtptp_i32_must_be_4[(sizeof(amtptp_i32) == 4u) ? 1 : -1];
typedef char amtptp_u8_must_be_1[(sizeof(amtptp_u8) == 1u) ? 1 : -1];
typedef char amtptp_u16_must_be_2[(sizeof(amtptp_u16) == 2u) ? 1 : -1];
typedef char amtptp_u32_must_be_4[(sizeof(amtptp_u32) == 4u) ? 1 : -1];

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
    amtptp_i16 absolute_x;
    amtptp_i16 absolute_y;
    amtptp_u8 finger;
    amtptp_u8 state;
    amtptp_u8 touch_major;
    amtptp_u8 touch_minor;
    amtptp_u8 size;
    amtptp_u8 pressure;
    amtptp_u8 id;
    amtptp_u8 orientation;
} amtptp_raw_contact;

typedef struct amtptp_raw_frame {
    amtptp_u32 timestamp_ms;
    amtptp_u8 button;
    amtptp_u8 contact_count;
    amtptp_raw_contact contacts[AMTPTP_MAX_RAW_CONTACTS];
} amtptp_raw_frame;

typedef struct amtptp_contact {
    amtptp_u32 id;
    amtptp_u16 x;
    amtptp_u16 y;
    amtptp_u8 confidence;
    amtptp_u8 tip_switch;
} amtptp_contact;

typedef struct amtptp_frame {
    amtptp_u16 scan_time;
    amtptp_u8 button;
    amtptp_u8 contact_count;
    amtptp_contact contacts[AMTPTP_MAX_PTP_CONTACTS];
} amtptp_frame;

typedef struct amtptp_options {
    amtptp_i16 x_min;
    amtptp_i16 y_min;
    amtptp_u16 x_max;
    amtptp_u16 y_max;
    amtptp_u32 stop_pressure;
    amtptp_u32 stop_size;
    amtptp_u8 button_disabled;
    amtptp_u8 ignore_button_finger;
    amtptp_u8 ignore_near_fingers;
    amtptp_u8 palm_rejection;
} amtptp_options;

typedef struct amtptp_locked_contact {
    amtptp_u32 id;
    amtptp_u16 x;
    amtptp_u16 y;
    amtptp_u8 tip_switch;
    amtptp_u8 locked;
} amtptp_locked_contact;

typedef struct amtptp_session {
    amtptp_u16 admitted_mask;
    amtptp_u16 suppressed_mask;
    amtptp_u8 previous_button;
    amtptp_locked_contact locked_contacts[2];
} amtptp_session;

void amtptp_default_options(amtptp_options *options);
void amtptp_reset_session(amtptp_session *session);

amtptp_status amtptp_decode_mt2(
    const amtptp_u8 *input,
    amtptp_size input_length,
    amtptp_raw_frame *output);

amtptp_status amtptp_convert_ptp(
    amtptp_session *session,
    const amtptp_options *options,
    const amtptp_raw_frame *input,
    amtptp_frame *output);

amtptp_status amtptp_serialize_ptp(
    const amtptp_frame *input,
    amtptp_u8 *output,
    amtptp_size output_capacity,
    amtptp_size *output_length);

#ifdef __cplusplus
}
#endif

#endif
