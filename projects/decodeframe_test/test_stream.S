/* EVO Player - decodeframe_test
 *
 * Links test_1080p.h264 into the payload as read-only data.
 *
 * WHY .incbin RATHER THAN A GENERATED C ARRAY
 *   A 290 KB byte array written as C source is about 1.5 MB of text, which is
 *   unpleasant to review, slow to compile and pointless to diff. .incbin puts
 *   the same bytes in .rodata with the file staying a file, so `ffprobe` and
 *   `ffmpeg` can be pointed straight at it and the generator script can
 *   replace it without touching any source.
 *
 * The path is relative to the assembler's working directory, which is this
 * project directory when the Makefile runs. The Makefile also passes -I$(CURDIR)
 * so an out-of-tree build still finds it.
 */

    .section .rodata
    .balign 16

    .globl  test_stream_h264
    .type   test_stream_h264, @object
test_stream_h264:
    .incbin "test_1080p.h264"
test_stream_h264_end:
    .size   test_stream_h264, test_stream_h264_end - test_stream_h264

    .balign 4
    .globl  test_stream_h264_len
    .type   test_stream_h264_len, @object
test_stream_h264_len:
    .int    test_stream_h264_end - test_stream_h264
    .size   test_stream_h264_len, 4

    .section .note.GNU-stack, "", @progbits
