/* Simulator fixture tests for the typed HTTP client boundary. */
#include "test_runner.h"
#include "thistle_http_shim.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint8_t data[128];
    int length;
} response_capture_t;

static int capture_response(const uint8_t *data, int data_len, void *user_data)
{
    response_capture_t *capture = user_data;
    if (!capture || !data || data_len < 0 || data_len > (int)sizeof(capture->data)) {
        return -1;
    }
    memcpy(capture->data, data, (size_t)data_len);
    capture->length = data_len;
    return 0;
}

TEST(http_shim_delivers_fixture_bytes_status_and_cleanup) {
    static const char fixture[] = "{\"apps\":[{\"id\":\"fixture\"}]}";
    static const char path[] = "/tmp/thistle-http-shim-fixture.json";
    static const char url[] = "file:///tmp/thistle-http-shim-fixture.json";

    FILE *file = fopen(path, "wb");
    ASSERT_TRUE(file != NULL);
    ASSERT_EQ((int)fwrite(fixture, 1, sizeof(fixture) - 1, file),
              (int)sizeof(fixture) - 1);
    ASSERT_EQ(fclose(file), 0);

    response_capture_t capture = {0};
    void *client = thistle_http_client_init(url, 2500, capture_response, &capture);
    ASSERT_TRUE(client != NULL);
    ASSERT_EQ(thistle_http_client_perform(client), 0);
    ASSERT_EQ(capture.length, (int)sizeof(fixture) - 1);
    ASSERT_EQ(memcmp(capture.data, fixture, sizeof(fixture) - 1), 0);
    /* libcurl file:// responses have no HTTP status, but the accessor must be safe. */
    ASSERT_EQ(thistle_http_client_get_status_code(client), 0);
    ASSERT_EQ(thistle_http_client_cleanup(client), 0);
    ASSERT_EQ(unlink(path), 0);
}

/* The streaming calls hand a body over as it is read, in pieces no larger
 * than asked for, and report the end once. */
TEST(http_shim_streams_a_body_in_pieces) {
    static const char path[] = "/tmp/thistle-http-shim-stream.bin";
    static const char url[] = "file:///tmp/thistle-http-shim-stream.bin";
    enum { SIZE = 300 * 1024 };

    static uint8_t body[SIZE];
    for (int i = 0; i < SIZE; i++) body[i] = (uint8_t)(i * 7 + (i >> 8));
    FILE *file = fopen(path, "wb");
    ASSERT_TRUE(file != NULL);
    ASSERT_EQ((int)fwrite(body, 1, SIZE, file), SIZE);
    ASSERT_EQ(fclose(file), 0);

    void *client = thistle_http_client_init(url, 2500, NULL, NULL);
    ASSERT_TRUE(client != NULL);
    ASSERT_EQ(thistle_http_client_open(client, 0), 0);
    ASSERT_EQ((int)thistle_http_client_fetch_headers(client), SIZE);

    static uint8_t got[SIZE];
    int total = 0, n;
    char piece[1000];
    while ((n = thistle_http_client_read(client, piece, sizeof piece)) > 0) {
        ASSERT_TRUE(n <= (int)sizeof piece);
        ASSERT_TRUE(total + n <= SIZE);
        memcpy(got + total, piece, (size_t)n);
        total += n;
    }
    ASSERT_EQ(n, 0);
    ASSERT_EQ(total, SIZE);
    ASSERT_EQ(memcmp(got, body, SIZE), 0);
    ASSERT_EQ(thistle_http_client_read(client, piece, sizeof piece), 0);

    /* Opened again, it starts over. */
    ASSERT_EQ(thistle_http_client_open(client, 0), 0);
    ASSERT_EQ((int)thistle_http_client_fetch_headers(client), SIZE);
    ASSERT_EQ(thistle_http_client_read(client, piece, 10), 10);
    ASSERT_EQ(memcmp(piece, body, 10), 0);
    ASSERT_EQ(thistle_http_client_close(client), 0);
    ASSERT_EQ(thistle_http_client_cleanup(client), 0);
    ASSERT_EQ(unlink(path), 0);
}

/* A file that is not there fails the headers, not a later read. */
TEST(http_shim_stream_of_a_missing_file_fails) {
    void *client = thistle_http_client_init("file:///tmp/thistle-http-shim-absent.bin", 2500, NULL, NULL);
    ASSERT_TRUE(client != NULL);
    ASSERT_EQ(thistle_http_client_open(client, 0), 0);
    ASSERT_TRUE(thistle_http_client_fetch_headers(client) < 0);
    ASSERT_EQ(thistle_http_client_cleanup(client), 0);
}
