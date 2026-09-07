#include "amtptp_source.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static size_t packet(unsigned char *out, unsigned count, unsigned first_id)
{
    unsigned i;
    memset(out, 0, 149);
    out[0] = 0xa1; out[1] = 0x31;
    for (i = 0; i < count; ++i) {
        unsigned offset = 5 + 9 * i;
        out[offset + 3] = 0x88; /* finger=2, state=4, x=y=0 */
        out[offset + 4] = 12;
        out[offset + 5] = 10;
        out[offset + 6] = 14;
        out[offset + 7] = 30;
        out[offset + 8] = (unsigned char)(first_id + i);
    }
    return 5 + 9 * count;
}

static void enable(amtptp_source *source)
{
    const unsigned char mode[] = {4,3};
    amtptp_report_batch discarded;
    CHECK(amtptp_source_set_feature(source, 4, mode, sizeof(mode)) == AMTPTP_SOURCE_REPORT);
    amtptp_source_release(source, &discarded);
}

static void features(void)
{
    amtptp_source source;
    unsigned char output[258];
    const unsigned char bad_mode[] = {4,2}, bad_id[] = {6,3}, disable[] = {6,0};
    size_t written;
    amtptp_source_init(&source);
    CHECK(amtptp_source_get_feature(&source, 4, output, sizeof(output), &written) == 1);
    CHECK(written == 2 && output[0] == 4 && output[1] == 0);
    CHECK(amtptp_source_get_feature(&source, 7, output, sizeof(output), &written) == 1);
    CHECK(written == 3 && output[1] == 5 && output[2] == 0);
    memset(output, 0xcc, sizeof(output));
    CHECK(amtptp_source_get_feature(&source, 8, output, 257, &written) == 1);
    CHECK(written == 257 && output[1] == 0xfc && output[256] == 0xc2 && output[257] == 0xcc);
    CHECK(amtptp_source_get_feature(&source, 8, output, 256, &written) == -1 && written == 0);
    CHECK(amtptp_source_set_feature(&source, 4, bad_mode, 2) == -1);
    CHECK(amtptp_source_set_feature(&source, 4, bad_id, 2) == -1);
    CHECK(source.input_mode == 0);
    CHECK(amtptp_source_set_feature(&source, 6, disable, 2) == 1);
    CHECK(source.surface_enabled == 0 && source.button_enabled == 0);
    CHECK(amtptp_source_set_feature(&source, 7, disable, 2) == -1);
}

static void gesture_contacts(void)
{
    amtptp_source source;
    amtptp_report_batch result;
    unsigned char input[149];
    size_t length;
    unsigned count, slot;
    amtptp_source_init(&source);
    length = packet(input, 2, 1);
    CHECK(amtptp_source_input(&source, input, length, &result) == 0 && result.count == 0);
    enable(&source);
    /* The native Windows gesture recognizer sees stable 2/3/4/5-contact frames. */
    for (count = 2; count <= 5; ++count) {
        length = packet(input, count, 1);
        CHECK(amtptp_source_input(&source, input, length, &result) == 1);
        CHECK(result.count == 1 && result.reports[0][48] == count);
        for (slot = 0; slot < count; ++slot) {
            CHECK(result.reports[0][1 + slot * 9] == 3);
            CHECK(result.reports[0][2 + slot * 9] == slot + 1);
        }
    }
    length = packet(input, 5, 2); /* ID 1 leaves while ID 6 joins a full surface. */
    CHECK(amtptp_source_input(&source, input, length, &result) == 1);
    CHECK(result.count == 2 && result.reports[0][48] == 5 && result.reports[1][48] == 5);
    CHECK(result.reports[0][2] == 1 && (result.reports[0][1] & 2) == 0);
    CHECK(result.reports[1][2] == 2);
    length = packet(input, 0, 0);
    CHECK(amtptp_source_input(&source, input, length, &result) == 1);
    CHECK(result.count == 2 && result.reports[0][48] == 5 && result.reports[1][48] == 0);
}

static void interruptions(void)
{
    amtptp_source source;
    amtptp_report_batch result;
    unsigned char input[149];
    const unsigned char battery[] = {0xa1,0x90,0,80}, unsupported[] = {0xa1,0x99};
    const unsigned char surface_off[] = {6,1}, button_off[] = {6,2};
    size_t length;
    amtptp_source_init(&source);
    enable(&source);
    length = packet(input, 3, 4); input[2] = 1;
    CHECK(amtptp_source_input(&source, input, length, &result) == 1 && result.reports[0][49] == 1);
    CHECK(amtptp_source_input(&source, battery, sizeof(battery), &result) == 0);
    CHECK(amtptp_source_input(&source, unsupported, sizeof(unsupported), &result) == 0);
    CHECK(source.previous.contact_count == 3 && source.previous.button == 1);
    amtptp_source_release(&source, &result);
    CHECK(result.count == 2 && result.reports[0][48] == 3 && result.reports[1][48] == 0);
    CHECK(result.reports[0][2] == 4 && (result.reports[0][1] & 2) == 0 && result.reports[0][49] == 0);
    CHECK(source.session.admitted_mask == 0 && source.previous.contact_count == 0);
    CHECK(amtptp_source_set_feature(&source, 6, surface_off, 2) == 1);
    amtptp_source_release(&source, &result);
    CHECK(amtptp_source_input(&source, input, length, &result) == 1);
    CHECK(result.reports[0][48] == 0 && result.reports[0][49] == 1);
    CHECK(amtptp_source_set_feature(&source, 6, button_off, 2) == 1);
    amtptp_source_release(&source, &result);
    CHECK(amtptp_source_input(&source, input, length, &result) == 1);
    CHECK(result.reports[0][48] == 3 && result.reports[0][49] == 0);
    CHECK(amtptp_source_input(&source, input, length - 1, &result) == -1);
    CHECK(source.previous.contact_count == 3); /* malformed input cannot reset a gesture */
    input[5 + 9 + 8] = input[5 + 8];
    CHECK(amtptp_source_input(&source, input, length, &result) == -1); /* duplicate contact IDs */
    CHECK(source.previous.contact_count == 3);
    input[0] = 0xb1;
    CHECK(amtptp_source_input(&source, input, length, &result) == -1); /* No fragment misparse. */
    { unsigned char ack = 0; CHECK(amtptp_source_handshake(&ack, 1));
      ack = 2; CHECK(amtptp_source_handshake(&ack, 1));
      ack = 3; CHECK(!amtptp_source_handshake(&ack, 1));
      CHECK(!amtptp_source_handshake(&ack, 0)); }
}

/* Interpret HID short items to check the wire contract, independently of the
 * C serialization. Descriptor acceptance by Windows is a separate runtime gate. */
static void descriptor(void)
{
    unsigned bits[3][256] = {{0}}, size = 0, count = 0, id = 0, depth = 0;
    size_t offset = 0;
    while (offset < amtptp_source_descriptor_size) {
        unsigned prefix = amtptp_source_descriptor[offset++];
        unsigned bytes = prefix & 3u, value = 0, i;
        if (bytes == 3) bytes = 4;
        CHECK(prefix != 0xfe && offset + bytes <= amtptp_source_descriptor_size);
        if (offset + bytes > amtptp_source_descriptor_size) return;
        for (i = 0; i < bytes; ++i) value |= (unsigned)amtptp_source_descriptor[offset++] << (8 * i);
        switch (prefix & 0xfcu) {
        case 0x74: size = value; break;
        case 0x94: count = value; break;
        case 0x84: id = value; CHECK(id < 256); break;
        case 0x80: bits[0][id] += size * count; break;
        case 0x90: bits[1][id] += size * count; break;
        case 0xb0: bits[2][id] += size * count; break;
        case 0xa0: ++depth; break;
        case 0xc0: CHECK(depth > 0); --depth; break;
        default: break;
        }
    }
    CHECK(depth == 0 && bits[0][5] == 392);
    CHECK(bits[2][4] == 8 && bits[2][6] == 8 && bits[2][7] == 16 && bits[2][8] == 2048);
    CHECK(sizeof(amtptp_enable_multitouch) == 4 && amtptp_enable_multitouch[0] == 0x53 && amtptp_enable_multitouch[1] == 0xf1);
}

int main(void)
{
    features(); gesture_contacts(); interruptions(); descriptor();
    if (failures) return 1;
    puts("All native PTP source tests passed (features, 2-5 contacts, lift, disconnect, selective reporting, framing, descriptor).");
    return 0;
}
