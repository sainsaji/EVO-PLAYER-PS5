/* EVO Player - avplayer_probe
 *
 * Links test_720p.mp4 into .rodata. The probe serves it to libSceAvPlayer
 * through the file-replacement callbacks, so nothing has to be copied onto the
 * console and the file-access model is observed rather than assumed - which is
 * the entire point of instrumenting those callbacks.
 */

    .section .rodata
    .balign 16

    .globl  test_clip_mp4
    .type   test_clip_mp4, @object
test_clip_mp4:
    .incbin "test_720p.mp4"
test_clip_mp4_end:
    .size   test_clip_mp4, test_clip_mp4_end - test_clip_mp4

    .balign 8
    .globl  test_clip_mp4_len
    .type   test_clip_mp4_len, @object
test_clip_mp4_len:
    .quad   test_clip_mp4_end - test_clip_mp4
    .size   test_clip_mp4_len, 8

    .section .note.GNU-stack, "", @progbits
