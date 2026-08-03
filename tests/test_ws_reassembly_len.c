/*
 *
 * Copyright (C) 2019-2025, Broadband Forum
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file test_ws_reassembly_len.c
 *
 * Regression test for the WebSocket reassembly length arithmetic used in
 * HandleWscEvent_Receive() (src/core/wsclient.c) and HandleWssEvent_Receive()
 * (src/core/wsserver.c).
 *
 * A malicious WebSocket peer controls the value returned by
 * lws_remaining_packet_payload() (it reflects the peer-declared 64-bit frame
 * length). The original code computed:
 *
 *     int new_len = wc->rx_buf_len + chunk_len + lws_remaining_packet_payload(handle);
 *
 * Performing the sum in a signed 32-bit int lets the peer choose a declared
 * length near 2^31 / 2^32 so that new_len wraps to a small positive (or
 * negative) value. That bypasses both the "> MAX_USP_MSG_LEN" guard and the
 * realloc-grow check, leaving rx_buf under-allocated for the following memcpy
 * of chunk_len attacker-controlled bytes -> heap overflow / OOB write.
 *
 * The fix performs the arithmetic in 64-bit (uint64_t) and validates against
 * MAX_USP_MSG_LEN *before* narrowing to the int buffer-length fields.
 *
 * This test exercises the length-guard idiom directly. It is intentionally
 * self-contained (no libwebsockets dependency) so it can run in any build,
 * including builds configured with --disable-websockets. Run it via the
 * accompanying script:
 *
 *     ./run_tests.sh            # runs the safe (fixed) variant  -> exits 0
 *     ./run_tests.sh vulnerable # runs the 32-bit (vulnerable) variant -> exits 1
 *
 * i.e. the test fails against the pre-fix arithmetic and passes against the
 * fixed 64-bit arithmetic.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_USP_MSG_LEN (5*1024*1024)

// Models the length-guard decision from HandleWsc/WssEvent_Receive.
// Returns true if the frame is accepted for reassembly (i.e. rx_buf will be
// grown to hold it), false if it is rejected as too large.
//
// SAFE variant mirrors the fixed code: 64-bit arithmetic + validation before
// any narrowing cast. VULNERABLE variant mirrors the original signed 32-bit
// arithmetic that this finding is about.
static bool frame_accepted(int rx_buf_len, int chunk_len, uint64_t remaining, int *out_new_len)
{
#ifdef VULNERABLE
    int new_len = rx_buf_len + chunk_len + (int)remaining;   // 32-bit truncating sum (the bug)
    if (new_len > MAX_USP_MSG_LEN)
    {
        return false;
    }
    *out_new_len = new_len;
    return true;
#else
    uint64_t total_len = (uint64_t)rx_buf_len + (uint64_t)chunk_len + remaining;
    if (total_len > MAX_USP_MSG_LEN)
    {
        return false;
    }
    *out_new_len = (int)total_len;
    return true;
#endif
}

typedef struct
{
    const char *name;
    int rx_buf_len;
    int chunk_len;
    uint64_t remaining;    // peer-controlled
    bool expect_accepted;  // whether reassembly should proceed
} testcase_t;

int main(void)
{
    // For an accepted frame, the allocated size (new_len) must be able to hold
    // the bytes about to be copied (rx_buf_len + chunk_len). If it cannot, the
    // memcpy overflows -> this is the heap-overflow condition we must prevent.
    testcase_t cases[] = {
        { "normal small frame",              0,      1024,   0,                       true  },
        { "legit fragmented frame",          1000,   1024,   4096,                    true  },
        { "exactly at limit",                0,      MAX_USP_MSG_LEN, 0,              true  },
        { "one over limit",                  0,      0,      MAX_USP_MSG_LEN + 1ULL,  false },
        // Attack: declared length ~2^32 so the 32-bit sum wraps to a small value,
        // but a real chunk of 4096 bytes is delivered.
        { "overflow wrap to small (2^32)",   0,      4096,   0x100000000ULL,          false },
        // Attack: declared length ~2^31 so the 32-bit sum goes negative.
        { "overflow wrap to negative (2^31)",0,      4096,   0x80000000ULL,           false },
        // Attack: chunk near limit + remaining that wraps the 32-bit sum small.
        { "overflow wrap with big chunk",    0,      MAX_USP_MSG_LEN, 0x100000000ULL, false },
    };

    int failures = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
    {
        testcase_t *tc = &cases[i];
        int new_len = 0;
        bool accepted = frame_accepted(tc->rx_buf_len, tc->chunk_len, tc->remaining, &new_len);

        bool ok = (accepted == tc->expect_accepted);

        // Critical safety invariant: if we accept the frame, the buffer we size
        // (new_len) MUST be large enough for the bytes copied by the memcpy.
        if (ok && accepted)
        {
            int64_t bytes_copied = (int64_t)tc->rx_buf_len + (int64_t)tc->chunk_len;
            if (new_len < bytes_copied)
            {
                ok = false;   // would under-allocate -> heap overflow
            }
        }

        printf("[%s] %-32s accepted=%d new_len=%d\n",
               ok ? "PASS" : "FAIL", tc->name, accepted, new_len);

        if (!ok)
        {
            failures++;
        }
    }

    if (failures != 0)
    {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }

    printf("\nAll tests passed\n");
    return 0;
}
