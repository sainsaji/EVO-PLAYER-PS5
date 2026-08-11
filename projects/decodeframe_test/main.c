/* EVO Player - decodeframe_test
 *
 * PHASE 6: decode one frame.
 *
 * Phase 5 built a decoder and tore it down cleanly. This is the first program
 * that gives the hardware something to do: it hands sceVideodec2Decode a real
 * H.264 access unit and asks whether a picture came back.
 *
 * RUN IT WITH ./scripts/install-homebrew.sh --run, NOT ./scripts/deploy.sh.
 * The 1080p working set is 90 MiB and the elfldr slot caps out below 64.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE PHASE PLAN SAID, AND WHAT READING THE MODULE CHANGED
 *
 * docs/hardware-decode-next-steps.md opens Phase 6 with "Phase 5 hands this
 * three things it did not have: ... a measured memInfo+0x38 (mapMemorySize)
 * that has no buffer of its own - which is very likely what MapMemory and
 * MapDirectMemory are for. Read them before calling them, per rule 1."
 *
 * They were read. Three things came out of it, and all three change the plan:
 *
 *   1. THE FRAME BUFFER IS AN ARGUMENT TO Decode - AND ALSO SOMETHING YOU MAP.
 *      sceVideodec2Decode takes FOUR arguments, not two:
 *
 *          int Decode(void *decoder, const InputData *au,
 *                     FrameBuffer *fb, OutputInfo *out);
 *
 *      The third one carries the output buffer. That much was read correctly
 *      offline. What was read WRONG - and what cost the first run - is the
 *      conclusion drawn from it: that the map calls were therefore not part of
 *      decoding. See below.
 *
 *   2. sceVideodec2MapDirectMemory IS MANDATORY BEFORE THE FIRST Decode.
 *      Established by the 2026-08-11 run, not by reading. Decode returned
 *      0x811D0111 - the generic "unexpected VdecCore code" bucket - and the
 *      module's own diagnostics named the cause:
 *
 *          [VDECCORE@B0A10C22:00000000]
 *          [SCEVDECCORE@A01D07A8:00000002]
 *          [SCEVIDEODEC2@A01A07A7:80C00001]
 *
 *      Innermost first. GpDec's line 0xC22 is a state check on the object at
 *      VdecCore+0x140:
 *
 *          mov r8d,[r14+0x40] ; cmp r8,5 ; ja ok
 *          mov eax,0x31 ; bt eax,r8d ; jae ok   ; states 0,4,5 refused
 *
 *      and it logged state 0. The only writes of 1 to that field come from a
 *      helper whose sole callers are both inside sceVdecCoreMapMemoryBlock -
 *      which is exactly what sceVideodec2MapDirectMemory calls.
 *
 *      NOTHING ON VIDEODEC2'S DECODE PATH CONSULTS A MAPPED FLAG. That reading
 *      was complete and correct, and still produced the wrong answer, because
 *      the state that gates input lives two layers below the API being read.
 *      Standing rules 16 and 17 were written for this.
 *
 *      Mapping is allowed only while the GpDec state is below 2, i.e. between
 *      CreateDecoder and the first Decode, and at most 16 blocks may be
 *      registered - the same 16 as the maximum DPB, which is what suggests the
 *      blocks ARE the output frame buffers.
 *
 *   2b. sceVideodec2MapMemory IS STILL NOT SAFE TO CALL. Its one import is
 *      unbound: the GOT slot (Videodec2 s2 +0x60) still points back into its
 *      own PLT push sequence, while every neighbouring slot that lands in
 *      libSceVdecCore is resolved. That is the shape of the Phase 4 hang, and
 *      here the slot never bound so there is not even a module name to load.
 *      MapDirectMemory is the bound one, and it is the one this probe calls.
 *
 *   3. mapMemorySize IS EXACTLY ONE OUTPUT FRAME, and the arithmetic is
 *      readable offline from Phase 5's own measurements:
 *
 *          mapMemorySize = align(width, 256) * align(height, N) * 3/2
 *                          + 5 * 1024
 *
 *      with N = 16 for H.264 and N = 1 for HEVC, and the 3/2 doubling to 3 for
 *      10-bit. It reproduces all eight figures Phase 5 recorded, to the byte:
 *
 *          AVC   1080p  2048*1088*3/2 + 5120 =  3,347,456  measured  3,347,456
 *          AVC    720p  1280* 720*3/2 + 5120 =  1,387,520  measured  1,387,520
 *          AVC      4K  3840*2160*3/2 + 5120 = 12,446,720  measured 12,446,720
 *          HEVC  1080p  2048*1080*3/2 + 5120 =  3,322,880  measured  3,322,880
 *          HEVC 10b 1080p          x2 - 5120 =  6,640,640  measured  6,640,640
 *
 *      So the pixel format is 8 or 16 bits per luma sample with half-height
 *      interleaved chroma - NV12 and P010 - the stride is the width rounded up
 *      to 256, and the height padding differs by codec. That is most of what
 *      Phase 7 was going to spend a deploy measuring, obtained for free, and
 *      the probe checks the prediction against the module on every config it
 *      queries so the arithmetic is confirmed rather than assumed.
 *
 *      The trailing 5,120 bytes are five 1 KiB blocks, and GetPictureInfo says
 *      what they are: it reads picture metadata from
 *      frameBuffer + frameBufferSize - pictureCount * 1024, walking backwards
 *      1 KiB per picture. The frame buffer carries its own metadata in its
 *      tail.
 *
 * ---------------------------------------------------------------------------
 * THE DECODE ABI, READ OFF THE MODULE BEFORE ANY OF IT WAS CALLED
 *
 * sceVideodec2Decode (+0x1290), in order:
 *
 *     [decoder+0x68] == 0xa824d9799010a455              -> 0x811D0103
 *     au, fb, out all non-NULL                          -> 0x811D0102
 *     (decoder+0x5c & ~1) != 0x24708                    -> 0x811D0103
 *     au->thisSize == 0x30                              -> 0x811D0101
 *     fb->thisSize == 0x20                              -> 0x811D0101
 *     out->thisSize == 0x30 or 0x38   (`or rax,8; cmp 0x38`)
 *                                                       -> 0x811D0101
 *     fb->frameBuffer non-NULL                          -> 0x811D0107
 *     fb->isAccepted = 0
 *     low byte of fb->frameBuffer clear, i.e. 256-aligned
 *                                                       -> 0x811D0108
 *     fb->frameBufferSize != 0                          -> 0x811D0106
 *     au->auData non-NULL                               -> 0x811D010E
 *     au->auSize != 0                                   -> 0x811D010D
 *     out->isValid = out->isErrorFrame = 0
 *     lock decoder+0xc8                                 -> 0x811D0111
 *     decoder+0x50 (flushed latch) == 0                 -> 0x811D0100
 *     decoder+0x48 == 0                                 -> 0x811D0300
 *     decoder+0x4c == 0                                 -> 0x811D0304
 *     sceVdecCoreSetDecodeInput(core, {au, fb}, &status, &pending)
 *     if (!pending) sceVdecCoreSyncDecode(core, &count)
 *                   then harvest count pictures via sceVdecCoreGetDecodeOutput
 *
 * THE FLUSHED LATCH IS ONE-WAY. sceVideodec2Flush sets decoder+0x50, and every
 * subsequent Decode returns 0x811D0100 until sceVideodec2Reset clears it. So
 * Flush goes last, and this probe creates a fresh decoder per framing attempt
 * rather than trying to recover one.
 *
 * FOUR OF THE EIGHTEEN EXPORTS ARE TWO PAIRS OF ALIASES. Standing rule 14 cost
 * Phase 5 an experiment design; it is worth restating with the new pairs:
 *
 *     sceVideodec2GetPictureInfo    (+0x2120)  xor ecx,ecx ; jmp +0x2130
 *     sceVideodec2GetAvcPictureInfo (+0x30c0)  xor ecx,ecx ; jmp +0x2130
 *          ^ byte-for-byte identical
 *     sceVideodec2GetHevcPictureInfo(+0x30e0)  xor edx,edx ; xor ecx,ecx ; jmp
 *     sceVideodec2GetVp9PictureInfo (+0x30d0)  xor edx,edx ; xor ecx,ecx ; jmp
 *          ^ byte-for-byte identical to each other
 *
 * There is one body. It dispatches on out->codecType, which the decoder wrote,
 * not on which name you called. The Hevc/Vp9 pair differ only in forcing the
 * second picture-info pointer to NULL. The probe resolves all four and logs
 * their offsets to record the aliasing rather than assert it.
 *
 * sceVideodec2GetPictureInfo(out, pic0, pic1), in order:
 *
 *     pic0 non-NULL                                     -> 0x811D0102
 *     pic0->thisSize field at +0x08 cleared; same for pic1 if given
 *     out->thisSize == 0x30 or 0x38                     -> 0x811D0101
 *     out->frameBuffer 256-aligned                      -> 0x811D0108
 *     out->frameBuffer non-NULL                         -> 0x811D0107
 *     out->pictureCount in 1..2                         -> 0x811D010F
 *     out->frameBufferSize > pictureCount * 1024        -> 0x811D0106
 *     per picture, metadata at frameBuffer + frameBufferSize - n*1024:
 *         AVC  (out->codecType 1)        pic->thisSize | 0x10 == 0x78
 *         HEVC (out->codecType 0xee049)  pic->thisSize == 0xa8
 *         VP9  (out->codecType 0x245bfd) pic->thisSize == 0x58
 *                                                       -> 0x811D0101
 *
 * ---------------------------------------------------------------------------
 * THE ONE QUESTION THE DISASSEMBLY COULD NOT ANSWER: BITSTREAM FRAMING
 *
 * Videodec2 passes auData and auSize straight through to VdecCore, which hands
 * them to the hardware. Nothing on the path scans for start codes, so nothing
 * on the path says whether the hardware wants Annex-B (00 00 01 prefixes) or
 * AVCC (4-byte big-endian NAL lengths). Reading further would mean following
 * the uvd_dec ioctl into the driver, which is not in the dump.
 *
 * So it is tested rather than guessed, both framings in one deploy, Annex-B
 * first because AvPlayer's own demuxer strings point that way. Each framing
 * gets its own decoder and its own buffers, so the second attempt cannot
 * inherit anything from the first.
 *
 * ---------------------------------------------------------------------------
 * SAFETY
 *   - Ordered cheapest-first (standing rule 9). Every control, the whole
 *     memory-arithmetic check, the compute queue and the decoder creation are
 *     logged before the first byte reaches the hardware. If the decode hangs,
 *     everything above it is still evidence and where it stopped is a finding.
 *   - sceVdecCoreSyncDecode blocks on the hardware. That is the hang risk in
 *     this phase and there is no in-payload guard against it: watchdog threads
 *     do not fire on 12.70. The guard is `timeout` around the deploy plus a
 *     log flushed after every line and written to /mnt/usb0.
 *   - MapMemory and MapDirectMemory are deliberately not called. See (2) above.
 *   - Nothing dereferences a pointer the module returned without first checking
 *     it lands inside a buffer this probe allocated. The decoder hands back a
 *     frame pointer; that pointer is range-checked before it is read.
 *   - Every decoder is deleted and every buffer released on every path.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"
#include "test_stream.h"

#define USB_DIR   "/mnt/usb0"
#define LOG_PATH  USB_DIR "/evo_decodeframe_log.txt"
#define FRAME_PATH USB_DIR "/evo_decodeframe_frame0.bin"

#define DMEM_ALIGN 0x20000u

#define MiB(x) ((size_t)(x) * 1024u * 1024u)

#define DECODER_CFG_SIZE       0x48
#define DECODER_MEMINFO_SIZE   0x48
#define COMPUTE_MEMINFO_SIZE   0x18
#define COMPUTE_QUEUEINFO_SIZE 0x10
#define INPUT_DATA_SIZE        0x30
#define FRAME_BUFFER_SIZE      0x20
#define OUTPUT_INFO_SIZE       0x38
#define AVC_PICTURE_INFO_SIZE  0x78

#define DECODER_OBJECT_SIZE 0x40000
#define DECODER_MAGIC1 0xa824d9799010a455ull

#define RES_STD 0xb6c8u
#define RES_BIG 0x12384u

#define CODEC_AVC  0x1u
#define CODEC_HEVC 0xee049u
#define CODEC_VP9  0x245bfdu

/* Phase 5's 1080p AVC dpb-16 depth-4 frame pool. The control: if this moved,
 * the module or the ABI reading changed and nothing below it means anything. */
#define PHASE5_1080P_FRAMEPOOL 86507776ull

/* Picture metadata is appended to the tail of the frame buffer, 1 KiB per
 * picture, which is where GetPictureInfo reads it from. */
#define PICTURE_META_SIZE 1024

static const char *const kModules[] = {
    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",
    "libSceGnmDriver.sprx",   /* sceGnmMapComputeQueue lives here           */
    "libSceSysmodule.sprx",   /* VdecCore loads its codec module through it  */
    "libSceAjm.sprx",
};

/* ------------------------------------------------------------------------- */
/* Structures                                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t thisSize;
    uint32_t resourceType;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    uint32_t maxFrameWidth;
    uint32_t maxFrameHeight;
    uint32_t maxDpbFrameCount;
    uint32_t decodePipelineDepth;
    void    *computeQueue;
    uint64_t cpuAffinityMask;
    int32_t  cpuThreadPriority;
    uint8_t  optimizeProgressiveVideo;
    uint8_t  checkMemoryType;
    uint8_t  extraDecoderLatency;
    uint8_t  enableStorageIntegrity;
    void    *extraConfigInfo;
} SceVideodec2DecoderConfigInfo;

typedef struct {
    uint64_t thisSize;
    uint64_t workMemorySize;
    void    *pWorkMemory;
    uint64_t frameMemorySize;
    void    *pFrameMemory;
    uint64_t extraMemorySize;
    void    *pExtraMemory;
    uint64_t mapMemorySize;
    uint32_t alignment;
    uint32_t reserved;
} SceVideodec2DecoderMemoryInfo;

typedef struct {
    uint64_t thisSize;
    uint64_t cpuGpuMemorySize;
    void    *cpuGpuMemory;
} SceVideodec2ComputeMemoryInfo;

typedef struct {
    uint64_t thisSize;
    uint16_t computePipeId;
    uint16_t computeQueueId;
    uint8_t  memoryCheckMode;
    uint8_t  reserved[3];
} SceVideodec2ComputeQueueInfo;

/* 0x30 bytes. Offsets [E] from Decode's argument marshalling, cross-checked
 * against sceVdecCoreSetDecodeInput's reads of the repacked struct. Names [I]. */
typedef struct {
    uint64_t thisSize;       /* +0x00  must be 0x30      else 0x811D0101 */
    const void *auData;      /* +0x08  non-NULL          else 0x811D010E */
    uint64_t auSize;         /* +0x10  non-zero          else 0x811D010D */
    uint64_t ptsData;        /* +0x18  passed through to VdecCore        */
    uint64_t dtsData;        /* +0x20  ditto - the pair moves as one xmm */
    uint64_t attachedData;   /* +0x28  ditto                             */
} SceVideodec2InputData;

/* 0x20 bytes. The output buffer for the decoded picture. */
typedef struct {
    uint64_t thisSize;       /* +0x00  must be 0x20      else 0x811D0101 */
    void    *frameBuffer;    /* +0x08  non-NULL 0x811D0107, 256-aligned
                              *        0x811D0108                        */
    uint64_t frameBufferSize;/* +0x10  non-zero          else 0x811D0106 */
    uint8_t  isAccepted;     /* +0x18  out; cleared on entry             */
    uint8_t  pad[7];
} SceVideodec2FrameBuffer;

/* 0x38 bytes (0x30 also accepted by Decode, but GetPictureInfo wants the two
 * extra words, so this probe always uses 0x38). Every offset [E] from the
 * harvest helper at +0x19d0; the names are the reading that fits them [I]. */
typedef struct {
    uint64_t thisSize;        /* +0x00  0x30 or 0x38                     */
    uint8_t  isValid;         /* +0x08  a picture came out               */
    uint8_t  isErrorFrame;    /* +0x09  set from two VdecCore error words */
    uint8_t  pictureCount;    /* +0x0a  1 or 2; GetPictureInfo demands it */
    uint8_t  streamState;     /* +0x0b                                   */
    uint32_t codecType;       /* +0x0c  1 / 0xee049 / 0x245bfd, remapped
                               *        from VdecCore's 0 / 4 / 6        */
    uint64_t ptsData;         /* +0x10                                   */
    uint32_t word18;          /* +0x18                                   */
    uint32_t pad1c;
    void    *frameBuffer;     /* +0x20  where the picture actually is    */
    uint64_t frameBufferSize; /* +0x28                                   */
    uint32_t word30;          /* +0x30  0 / 0xd460 / 0xc24a, size 0x38 only */
    uint32_t word34;          /* +0x34                     size 0x38 only */
} SceVideodec2OutputInfo;

/* 0x78 bytes. thisSize | 0x10 must equal 0x78, so 0x68 is also accepted.
 * The body copies a large number of AVC slice-header fields out of the 1 KiB
 * metadata block; this probe dumps the struct raw rather than pretending to
 * name fields it has not verified. The two it does name are the ones the body
 * copies as whole aligned words from the front of the metadata block, which is
 * where dimensions would sit. */
typedef struct {
    uint64_t thisSize;        /* +0x00  0x68 or 0x78                     */
    uint32_t isValid;         /* +0x08  set to 1 when filled             */
    uint32_t pad0c;
    uint8_t  raw[AVC_PICTURE_INFO_SIZE - 0x10];
} SceVideodec2AvcPictureInfo;

_Static_assert(sizeof(SceVideodec2DecoderConfigInfo) == DECODER_CFG_SIZE, "cfg 0x48");
_Static_assert(sizeof(SceVideodec2DecoderMemoryInfo) == DECODER_MEMINFO_SIZE, "mem 0x48");
_Static_assert(sizeof(SceVideodec2InputData) == INPUT_DATA_SIZE, "input 0x30");
_Static_assert(sizeof(SceVideodec2FrameBuffer) == FRAME_BUFFER_SIZE, "fb 0x20");
_Static_assert(sizeof(SceVideodec2OutputInfo) == OUTPUT_INFO_SIZE, "out 0x38");
_Static_assert(sizeof(SceVideodec2AvcPictureInfo) == AVC_PICTURE_INFO_SIZE, "pic 0x78");
_Static_assert(__builtin_offsetof(SceVideodec2InputData, auSize) == 0x10, "auSize +0x10");
_Static_assert(__builtin_offsetof(SceVideodec2FrameBuffer, isAccepted) == 0x18,
               "isAccepted +0x18");
_Static_assert(__builtin_offsetof(SceVideodec2OutputInfo, frameBuffer) == 0x20,
               "frameBuffer +0x20");
_Static_assert(__builtin_offsetof(SceVideodec2OutputInfo, pictureCount) == 0x0a,
               "pictureCount +0x0a");

/* 0x20 bytes. Read off sceVideodec2MapDirectMemory (+0x340), which repacks it
 * into sceVdecCoreMapMemoryBlock's 0x20-byte entry as
 * {addr, size, physAddr, mode=0} and passes a count of 1.
 *
 * THE FIELD ORDER IS NOT THE OBVIOUS ONE - size comes before the pointer. It
 * is settled by what VdecCore does with the repacked entry, not by guessing:
 *
 *     mov  rdx, [rsi+0x8]        ; entry+0x08
 *     mov  r13, [rsi]            ; entry+0x00
 *     mov  rdi, rdx
 *     not  rdi
 *     cmp  r13, rdi              ; base <= ~len  - an overflow check
 *     ...
 *     lea  r15, [rdx+r13*1]      ; end = base + len
 *
 * so entry+0x00 is the base and entry+0x08 is the length. Videodec2 fills
 * entry+0x00 from info+0x10 and entry+0x08 from info+0x08, which puts the
 * length at info+0x08 - and that is corroborated by the error it raises when
 * info+0x08 is zero: 0x811D0104, "a caller size is below the computed
 * requirement". A size, not a pointer.
 *
 * info+0x18 lands in the block's slot at +0x08 alongside base and length.
 * "DirectMemory" in the name plus a slot that wants a third value next to a
 * CPU address makes the direct-memory physical offset the obvious reading
 * [H] - so the probe tries the physical address first and falls back to zero,
 * rather than asserting either. */
typedef struct {
    uint64_t thisSize;   /* +0x00  exactly 0x20        else 0x811D0101 */
    uint64_t size;       /* +0x08  non-zero            else 0x811D0104 */
    void    *addr;       /* +0x10  CPU virtual address                 */
    uint64_t physAddr;   /* +0x18  [H] direct-memory physical offset   */
} SceVideodec2MapDirectMemoryInfo;

_Static_assert(sizeof(SceVideodec2MapDirectMemoryInfo) == 0x20, "mapinfo 0x20");

typedef int (*map_direct_memory_fn)(void *decoder,
                                    const SceVideodec2MapDirectMemoryInfo *);

typedef int (*query_decoder_meminfo_fn)(const SceVideodec2DecoderConfigInfo *,
                                        SceVideodec2DecoderMemoryInfo *);
typedef int (*create_decoder_fn)(const SceVideodec2DecoderConfigInfo *,
                                 SceVideodec2DecoderMemoryInfo *, void **);
typedef int (*delete_decoder_fn)(void *);
typedef int (*decode_fn)(void *decoder, const SceVideodec2InputData *,
                         SceVideodec2FrameBuffer *, SceVideodec2OutputInfo *);
typedef int (*flush_fn)(void *decoder, SceVideodec2FrameBuffer *,
                        SceVideodec2OutputInfo *);
typedef int (*reset_fn)(void *decoder);
typedef int (*get_picture_info_fn)(const SceVideodec2OutputInfo *,
                                   void *pic0, void *pic1);
typedef int (*query_compute_meminfo_fn)(SceVideodec2ComputeMemoryInfo *);
typedef int (*alloc_compute_queue_fn)(const SceVideodec2ComputeQueueInfo *,
                                      SceVideodec2ComputeMemoryInfo *, void **);
typedef int (*release_compute_queue_fn)(void *);

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static FILE *g_log;

/* The format attribute is not decoration. Without it clang cannot check LOG's
 * arguments, and an argument left behind by an edit passes the build silently -
 * which happened once while writing this file. */
static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
LOG(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);   /* the only diagnostic that has survived a hang */
    }
}

static void
hexdump(const void *p, size_t len, const char *indent)
{
    const uint8_t *b = p;

    for (size_t i = 0; i < len; i += 16) {
        char line[200];
        int  n = snprintf(line, sizeof line, "%s%04zx  ", indent, i);

        for (size_t j = 0; j < 16 && i + j < len; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, "%02x ", b[i + j]);
        n += snprintf(line + n, sizeof line - (size_t)n, " |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = b[i + j];
            n += snprintf(line + n, sizeof line - (size_t)n, "%c",
                          (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        LOG("%s|\n", line);
    }
}

static const char *
vdec2_err(uint32_t rc)
{
    switch (rc) {
    case 0x00000000: return "OK";
    case 0x811d0100: return "internal failure below the API / decoder flushed";
    case 0x811d0101: return "wrong struct size";
    case 0x811d0102: return "bad pointer (module argument validation)";
    case 0x811d0103: return "not a decoder handle";
    case 0x811d0104: return "a caller size is below the computed requirement";
    case 0x811d0105: return "memory check failed / NULL buffer";
    case 0x811d0106: return "size below the computed requirement / frame buffer too small";
    case 0x811d0107: return "frame buffer pointer rejected";
    case 0x811d0108: return "frame buffer not 256-byte aligned";
    case 0x811d0109: return "wrong memory type - wanted WB_ONION";
    case 0x811d010a: return "wrong memory type - wanted WC_GARLIC";
    case 0x811d010b: return "memory not writable";
    case 0x811d010c: return "reserved field not zero";
    case 0x811d010d: return "auSize is zero";
    case 0x811d010e: return "auData is NULL";
    case 0x811d010f: return "pictureCount outside 1..2";
    case 0x811d0110: return "release failed";
    case 0x811d0111: return "internal lock/unlock or unexpected VdecCore code";
    case 0x811d01ff: return "GnmDriver returned 0x80C00015";
    case 0x811d0200: return "invalid configuration (size computation refused)";
    case 0x811d0203: return "unsupported resource type (cfg+0x08)";
    case 0x811d0204: return "unsupported codec type (cfg+0x0c)";
    case 0x811d0205: return "unsupported profile or level for this codec";
    case 0x811d0206: return "decodePipelineDepth outside 1..8";
    case 0x811d0207: return "cpuAffinityMask out of range";
    case 0x811d0208: return "cpuThreadPriority outside 256..767";
    case 0x811d0209: return "maxDpbFrameCount outside 1..16";
    case 0x811d020b: return "extraConfigInfo must be NULL on this path";
    case 0x811d020c: return "VdecCore returned 0x80C00004";
    case 0x811d0300: return "decoder in error state (+0x48)";
    case 0x811d0301: return "VdecCore stream error";
    case 0x811d0302: return "VdecCore stream error";
    case 0x811d0303: return "VdecCore stream error";
    case 0x811d0304: return "decoder in error state (+0x4c)";
    case 0x80020023: return "EAGAIN - out of direct memory budget";
    default:         return "";
    }
}

static intptr_t
resolve(uint32_t dynh, intptr_t base, const char *name, unsigned expect_off)
{
    char     nid[12] = {0};
    intptr_t addr;

    nid_encode(name, nid);
    addr = kernel_dynlib_resolve(getpid(), dynh, nid);

    if (!addr) {
        LOG("  %-38s %s  DID NOT RESOLVE\n", name, nid);
        return 0;
    }
    LOG("  %-38s %s  +0x%-6lx %s\n", name, nid,
        (unsigned long)(addr - base),
        (unsigned long)(addr - base) == expect_off ? "ok" : "*** MOVED ***");
    return addr;
}

/* ------------------------------------------------------------------------- */
/* Direct memory                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    intptr_t paddr;
    void    *vaddr;
    size_t   len;
} dmem_block;

static size_t
round_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static int
dmem_get(dmem_block *b, size_t len, int memtype)
{
    intptr_t paddr = 0;
    void    *vaddr = NULL;
    int      rc;

    memset(b, 0, sizeof *b);
    len = round_up(len, DMEM_ALIGN);

    rc = sceKernelAllocateMainDirectMemory(len, DMEM_ALIGN, memtype, &paddr);
    if (rc)
        return rc;

    rc = sceKernelMapDirectMemory(&vaddr, len,
                                  SCE_KERNEL_PROT_CPU_RW | SCE_KERNEL_PROT_GPU_ALL,
                                  0, paddr, DMEM_ALIGN);
    if (rc) {
        sceKernelReleaseDirectMemory(paddr, len);
        return rc;
    }

    b->paddr = paddr;
    b->vaddr = vaddr;
    b->len   = len;
    return 0;
}

static void
dmem_put(dmem_block *b)
{
    if (b->vaddr)
        munmap(b->vaddr, b->len);
    if (b->paddr)
        sceKernelReleaseDirectMemory(b->paddr, b->len);
    memset(b, 0, sizeof *b);
}

/* The decoder hands back a frame pointer of its own choosing. Standing rule:
 * never speculatively dereference memory we did not allocate. This says
 * whether a module-supplied pointer lands inside one of our blocks, and how
 * many bytes are readable from it. */
static size_t
readable_from(const dmem_block *blocks, size_t n, const void *p,
              const char **which)
{
    static const char *const kNames[] = { "work", "frame pool", "input" };
    uintptr_t a = (uintptr_t)p;

    for (size_t i = 0; i < n; i++) {
        uintptr_t lo = (uintptr_t)blocks[i].vaddr;

        if (!lo || a < lo || a >= lo + blocks[i].len)
            continue;
        if (which) {
            static char label[32];

            if (i < 3) {
                *which = kNames[i];
            } else {
                snprintf(label, sizeof label, "mapped block %zu", i - 3);
                *which = label;
            }
        }
        return (size_t)(lo + blocks[i].len - a);
    }
    if (which)
        *which = "OUTSIDE every buffer we allocated";
    return 0;
}

/* ------------------------------------------------------------------------- */
/* The frame-size arithmetic, derived offline from Phase 5's measurements     */
/* ------------------------------------------------------------------------- */

/* Predicts memInfo+0x38. Confirmed against all eight Phase 5 figures offline;
 * the probe re-checks it against the module on every configuration it queries,
 * so a firmware that computes it differently shows up as a mismatch rather
 * than as corruption three phases later. */
static uint64_t
predict_map_memory_size(uint32_t codec, uint32_t profile, uint32_t w, uint32_t h)
{
    uint64_t stride = round_up(w, 256);
    uint64_t rows   = (codec == CODEC_AVC) ? round_up(h, 16) : h;
    uint64_t bytes  = (codec == CODEC_HEVC && profile == 2) ? 2 : 1;

    return stride * rows * bytes * 3 / 2 + 5 * PICTURE_META_SIZE;
}

/* ------------------------------------------------------------------------- */
/* Bitstream framing                                                         */
/* ------------------------------------------------------------------------- */

/* Rewrites one Annex-B access unit into AVCC: each start code is replaced by
 * the 4-byte big-endian length of the NAL that follows it. Returns the number
 * of bytes written, or 0 if the input did not look like Annex-B.
 *
 * This exists because nothing on the decode path scans for start codes, so
 * nothing offline says which framing the hardware wants. Both are tried. */
static size_t
annexb_to_avcc(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap)
{
    size_t starts[64];
    size_t nStarts = 0;
    size_t written = 0;

    for (size_t i = 0; i + 3 <= inLen && nStarts < 64; i++) {
        if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1)
            starts[nStarts++] = i + 3;
    }
    if (!nStarts)
        return 0;

    for (size_t k = 0; k < nStarts; k++) {
        size_t payload = starts[k];
        size_t end     = (k + 1 < nStarts) ? starts[k + 1] - 3 : inLen;

        /* Trim the extra leading zero of a 4-byte start code, and any trailing
         * zero bytes that belong to the next start code rather than this NAL. */
        while (end > payload && in[end - 1] == 0)
            end--;

        size_t nalLen = end - payload;
        if (written + 4 + nalLen > outCap)
            return 0;

        out[written + 0] = (uint8_t)(nalLen >> 24);
        out[written + 1] = (uint8_t)(nalLen >> 16);
        out[written + 2] = (uint8_t)(nalLen >> 8);
        out[written + 3] = (uint8_t)(nalLen);
        memcpy(out + written + 4, in + payload, nalLen);
        written += 4 + nalLen;
    }
    return written;
}

/* ------------------------------------------------------------------------- */
/* Arbitration                                                               */
/* ------------------------------------------------------------------------- */

/* libSceVideoDecoderArbitration, read off the module before being called.
 * AvPlayer drives this before it decodes anything; Route B never has, and the
 * Phase 5 discriminator table predicted in advance that a decoder which
 * CREATES but will not DECODE is what an unarbitrated client would look like.
 *
 * sceVideoDecoderArbitrationInitialize(const Params *):
 *     +0x00  thisSize, exactly 0x18            else 0x81570001
 *     +0x08  priority; (v - 0x300) unsigned must be >= 0xFFFFFE00,
 *            i.e. v in 256..767                else 0x81570004
 *     +0x10  count/mask; (v - 0x80) must be >= -0x7F,
 *            i.e. v in 1..127                  else 0x81570001
 *   A module-global "already initialised" byte gives 0x81570002 on a second
 *   call. The priority range is the same 256..767 that sceAvPlayerInit clamps
 *   its basePriority into and that the decoder config checks at cfg+0x38 -
 *   three independently-read functions agreeing on one range.
 *
 * sceVideoDecoderArbitrationEnable(NULL, callback):
 *     test rdi,rdi ; setne al
 *     test rsi,rsi ; sete  cl
 *     or   cl,al   ; jne error
 *   so the FIRST argument must be NULL and the second must NOT be. The second
 *   is stored in a module global and tail-called through a trampoline, so it
 *   is a callback pointer, not a handle. Enabling twice gives 0x81570002.
 *
 * sceVideoDecoderArbitrationAcceptEvent(unsigned): the argument must be <= 1,
 * else 0x81570001 with no side effect.
 */

#define ARB_PARAMS_SIZE 0x18

typedef struct {
    uint64_t thisSize;   /* +0x00  exactly 0x18   */
    uint32_t priority;   /* +0x08  256..767       */
    uint32_t pad0c;
    uint64_t count;      /* +0x10  1..127         */
} SceVideoDecoderArbitrationParams;

_Static_assert(sizeof(SceVideoDecoderArbitrationParams) == ARB_PARAMS_SIZE,
               "arbitration params must be 0x18 bytes");

typedef int (*arb_init_fn)(const SceVideoDecoderArbitrationParams *);
typedef int (*arb_enable_fn)(void *mustBeNull, void *callback);
typedef int (*arb_accept_fn)(unsigned int);

static volatile unsigned g_arb_events;

/* Registered with Enable and tail-called through the module's trampoline.
 * Deliberately does nothing but count: it may run on an arbitration thread,
 * and a logging call from an unknown context is not worth the risk. */
static void
arbitration_callback(void)
{
    g_arb_events++;
}

static const char *
arb_err(uint32_t rc)
{
    switch (rc) {
    case 0x00000000: return "OK";
    case 0x81570001: return "bad argument";
    case 0x81570002: return "already initialised / already enabled";
    case 0x81570004: return "priority out of range";
    case 0x815700ff: return "the layer below refused";
    default:         return "";
    }
}

/* Returns 1 if arbitration is initialised and enabled. Every failure is a
 * plain error return - nothing here allocates or touches hardware - so a
 * refusal costs the log line and nothing else. */
static int
arbitration_bring_up(uint32_t dynh_arb, intptr_t arb_base)
{
    static const uint64_t kCounts[] = { 1, 4, 0x7f };
    arb_init_fn   init;
    arb_enable_fn enable;
    arb_accept_fn accept;
    intptr_t a_init, a_enable, a_accept, a_suspend;
    int rc, ok = 0;

    LOG("\n--- arbitration ---\n");
    LOG("    Never called by this project before. AvPlayer calls Initialize\n"
        "    and Enable before it decodes, and the Phase 5 discriminator table\n"
        "    predicted that \"creates but will not decode\" is what an\n"
        "    unarbitrated client looks like. This is that experiment.\n\n");

    a_init    = resolve(dynh_arb, arb_base,
                        "sceVideoDecoderArbitrationInitialize", 0xf0);
    a_enable  = resolve(dynh_arb, arb_base,
                        "sceVideoDecoderArbitrationEnable", 0x1e0);
    a_accept  = resolve(dynh_arb, arb_base,
                        "sceVideoDecoderArbitrationAcceptEvent", 0x2a0);
    a_suspend = resolve(dynh_arb, arb_base,
                        "sceVideoDecoderArbitrationEnableSuspendMode", 0x270);
    (void)a_suspend;   /* resolved to record its offset; not called */

    if (!a_init || !a_enable || !a_accept) {
        LOG("  an arbitration entry point did not resolve - skipping\n");
        return 0;
    }

    init   = (arb_init_fn)a_init;
    enable = (arb_enable_fn)a_enable;
    accept = (arb_accept_fn)a_accept;

    /* -- controls, all free and all predicted ------------------------------ */
    {
        SceVideoDecoderArbitrationParams p;

        memset(&p, 0, sizeof p);
        p.thisSize = 0x10;
        p.priority = 700;
        p.count    = 1;
        rc = init(&p);
        LOG("  control: thisSize 0x10      -> 0x%08x expect 0x81570001  %s\n",
            rc, (uint32_t)rc == 0x81570001 ? "ok" : "*** DIFFERS ***");

        p.thisSize = ARB_PARAMS_SIZE;
        p.priority = 0x300;
        rc = init(&p);
        LOG("  control: priority 768       -> 0x%08x expect 0x81570004  %s\n",
            rc, (uint32_t)rc == 0x81570004 ? "ok" : "*** DIFFERS ***");

        p.priority = 700;
        p.count    = 0;
        rc = init(&p);
        LOG("  control: count 0            -> 0x%08x expect 0x81570001  %s\n",
            rc, (uint32_t)rc == 0x81570001 ? "ok" : "*** DIFFERS ***");

        rc = init(NULL);
        LOG("  control: NULL params        -> 0x%08x expect 0x81570001  %s\n",
            rc, (uint32_t)rc == 0x81570001 ? "ok" : "*** DIFFERS ***");

        rc = enable((void *)&p, (void *)arbitration_callback);
        LOG("  control: Enable(non-NULL,cb)-> 0x%08x expect 0x81570001  %s\n",
            rc, (uint32_t)rc == 0x81570001 ? "ok" : "*** DIFFERS ***");

        rc = enable(NULL, NULL);
        LOG("  control: Enable(NULL,NULL)  -> 0x%08x expect 0x81570001  %s\n",
            rc, (uint32_t)rc == 0x81570001 ? "ok" : "*** DIFFERS ***");

        rc = (int)accept(2);
        LOG("  control: AcceptEvent(2)     -> 0x%08x expect 0x81570001  %s\n",
            rc, (uint32_t)rc == 0x81570001 ? "ok" : "*** DIFFERS ***");
    }

    /* -- the real Initialize ---------------------------------------------- *
     * +0x10's meaning is [H]: the range 1..127 fits a small count or a CPU
     * mask and the disassembly does not say which. Laddered rather than
     * guessed - each rung is an error return, not a hang. */
    for (size_t i = 0; i < sizeof kCounts / sizeof *kCounts; i++) {
        SceVideoDecoderArbitrationParams p;

        memset(&p, 0, sizeof p);
        p.thisSize = ARB_PARAMS_SIZE;
        p.priority = 700;
        p.count    = kCounts[i];

        rc = init(&p);
        LOG("  Initialize(priority 700, +0x10 = %llu) -> 0x%08x %s\n",
            (unsigned long long)kCounts[i], rc, arb_err((uint32_t)rc));

        if (rc == 0 || (uint32_t)rc == 0x81570002) {
            ok = 1;
            break;
        }
    }

    if (!ok) {
        LOG("  Initialize refused at every value - not enabling\n");
        return 0;
    }

    rc = enable(NULL, (void *)arbitration_callback);
    LOG("  Enable(NULL, callback) -> 0x%08x %s\n", rc, arb_err((uint32_t)rc));
    if (rc && (uint32_t)rc != 0x81570002)
        return 0;

    rc = (int)accept(0);
    LOG("  AcceptEvent(0) -> 0x%08x %s\n", rc, arb_err((uint32_t)rc));
    rc = (int)accept(1);
    LOG("  AcceptEvent(1) -> 0x%08x %s\n", rc, arb_err((uint32_t)rc));

    LOG("  arbitration is up; callbacks seen so far: %u\n", g_arb_events);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

static void
cfg_init(SceVideodec2DecoderConfigInfo *cfg, uint32_t resourceType,
         uint32_t codecType, uint32_t profile, uint32_t level,
         uint32_t w, uint32_t h, uint32_t dpb, uint32_t depth, void *queue)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->thisSize            = DECODER_CFG_SIZE;
    cfg->resourceType        = resourceType;
    cfg->codecType           = codecType;
    cfg->profile             = profile;
    cfg->maxLevel            = level;
    cfg->maxFrameWidth       = w;
    cfg->maxFrameHeight      = h;
    cfg->maxDpbFrameCount    = dpb;
    cfg->decodePipelineDepth = depth;
    cfg->computeQueue        = queue;
    cfg->cpuThreadPriority   = 700;
}

/* ------------------------------------------------------------------------- */
/* Controls                                                                  */
/* ------------------------------------------------------------------------- */

/* Every one of these returns before sceVdecCoreSetDecodeInput is reached, so
 * none of them touches the hardware. They cost nothing and they are what turns
 * a later refusal into a diagnosis instead of a second deploy (rule 3). */
static void
decode_controls(decode_fn decode, void *decoder,
                const SceVideodec2InputData *goodAu,
                const SceVideodec2FrameBuffer *goodFb)
{
    static const struct {
        const char *label;
        uint64_t    auSize, fbSize, outSize;
        int         nullArgs, nullDecoder, nullFb, misalignFb, zeroFbLen;
        int         nullAu, zeroAuLen;
        uint32_t    expect;
    } kControls[] = {
        { "all-NULL",             0x30, 0x20, 0x38, 1,0,0,0,0, 0,0, 0x811d0102 },
        { "decoder = &junk",      0x30, 0x20, 0x38, 0,1,0,0,0, 0,0, 0x811d0103 },
        { "inputData size 0x28",  0x28, 0x20, 0x38, 0,0,0,0,0, 0,0, 0x811d0101 },
        { "frameBuffer size 0x18",0x30, 0x18, 0x38, 0,0,0,0,0, 0,0, 0x811d0101 },
        { "outputInfo size 0x28", 0x30, 0x20, 0x28, 0,0,0,0,0, 0,0, 0x811d0101 },
        { "outputInfo size 0x30", 0x30, 0x20, 0x30, 0,0,0,0,0, 0,0, 0x000000ff },
        { "frameBuffer NULL",     0x30, 0x20, 0x38, 0,0,1,0,0, 0,0, 0x811d0107 },
        { "frameBuffer +1",       0x30, 0x20, 0x38, 0,0,0,1,0, 0,0, 0x811d0108 },
        { "frameBufferSize 0",    0x30, 0x20, 0x38, 0,0,0,0,1, 0,0, 0x811d0106 },
        { "auData NULL",          0x30, 0x20, 0x38, 0,0,0,0,0, 1,0, 0x811d010e },
        { "auSize 0",             0x30, 0x20, 0x38, 0,0,0,0,0, 0,1, 0x811d010d },
    };
    uint64_t junk[16] = {0};

    LOG("\n--- Decode: validation controls ---\n");
    LOG("    None of these reaches sceVdecCoreSetDecodeInput, so none touches\n"
        "    the hardware. The \"outputInfo size 0x30\" row has no predicted\n"
        "    code on purpose: Decode accepts 0x30 and 0x38 alike, so it should\n"
        "    get PAST validation and behave like a real call. It is listed to\n"
        "    record which, not to assert a value.\n");

    for (size_t i = 0; i < sizeof kControls / sizeof *kControls; i++) {
        SceVideodec2InputData   au = *goodAu;
        SceVideodec2FrameBuffer fb = *goodFb;
        SceVideodec2OutputInfo  out;
        int rc;

        memset(&out, 0, sizeof out);
        out.thisSize = kControls[i].outSize;
        au.thisSize  = kControls[i].auSize;
        fb.thisSize  = kControls[i].fbSize;

        if (kControls[i].nullFb)
            fb.frameBuffer = NULL;
        if (kControls[i].misalignFb)
            fb.frameBuffer = (uint8_t *)fb.frameBuffer + 1;
        if (kControls[i].zeroFbLen)
            fb.frameBufferSize = 0;
        if (kControls[i].nullAu)
            au.auData = NULL;
        if (kControls[i].zeroAuLen)
            au.auSize = 0;

        /* The "outputInfo size 0x30" row is a real decode; skip it here so the
         * controls stay side-effect free and the first hardware call is the
         * one the log announces. */
        if (kControls[i].expect == 0x000000ff) {
            LOG("  %-24s -> not run; 0x30 is accepted, so this would be a real\n"
                "                           decode and the controls must stay inert\n",
                kControls[i].label);
            continue;
        }

        if (kControls[i].nullArgs)
            rc = decode(decoder, NULL, NULL, NULL);
        else if (kControls[i].nullDecoder)
            rc = decode(junk, &au, &fb, &out);
        else
            rc = decode(decoder, &au, &fb, &out);

        LOG("  %-24s -> 0x%08x  expect 0x%08x  %s\n",
            kControls[i].label, rc, kControls[i].expect,
            (uint32_t)rc == kControls[i].expect ? "ok" : "*** DIFFERS ***");
    }
}

/* GetPictureInfo validates entirely from the OutputInfo the caller hands it,
 * so its controls need no decoded picture at all - they run before the first
 * Decode. */
static void
picture_info_controls(get_picture_info_fn getPic, void *scratch)
{
    static const struct {
        const char *label;
        uint64_t    outSize;
        uint8_t     pictureCount;
        uint64_t    fbSize;
        uint64_t    picSize;
        int         nullPic, nullFb, misalignFb;
        uint32_t    expect;
    } kControls[] = {
        { "pic0 NULL",             0x38, 1, 1u << 20, 0x78, 1,0,0, 0x811d0102 },
        { "outputInfo size 0x28",  0x28, 1, 1u << 20, 0x78, 0,0,0, 0x811d0101 },
        { "frameBuffer NULL",      0x38, 1, 1u << 20, 0x78, 0,1,0, 0x811d0107 },
        { "frameBuffer +1",        0x38, 1, 1u << 20, 0x78, 0,0,1, 0x811d0108 },
        { "pictureCount 0",        0x38, 0, 1u << 20, 0x78, 0,0,0, 0x811d010f },
        { "pictureCount 3",        0x38, 3, 1u << 20, 0x78, 0,0,0, 0x811d010f },
        { "frameBufferSize 1024",  0x38, 1, 1024,     0x78, 0,0,0, 0x811d0106 },
        { "avc pictureInfo 0x70",  0x38, 1, 1u << 20, 0x70, 0,0,0, 0x811d0101 },
    };

    LOG("\n--- GetPictureInfo: validation controls ---\n");
    LOG("    These validate purely from the OutputInfo the caller supplies, so\n"
        "    they need no decoded picture and run before the hardware is used.\n"
        "    Every one of them returns before the module dereferences the\n"
        "    metadata pointer it computes - the picture-info size check comes\n"
        "    first - so none of them reads uninitialised memory.\n");

    for (size_t i = 0; i < sizeof kControls / sizeof *kControls; i++) {
        SceVideodec2OutputInfo     out;
        SceVideodec2AvcPictureInfo pic;
        int rc;

        memset(&out, 0, sizeof out);
        memset(&pic, 0, sizeof pic);
        out.thisSize        = kControls[i].outSize;
        out.isValid         = 1;
        out.pictureCount    = kControls[i].pictureCount;
        out.codecType       = CODEC_AVC;
        out.frameBuffer     = scratch;
        out.frameBufferSize = kControls[i].fbSize;
        pic.thisSize        = kControls[i].picSize;

        if (kControls[i].nullFb)
            out.frameBuffer = NULL;
        if (kControls[i].misalignFb)
            out.frameBuffer = (uint8_t *)scratch + 1;

        rc = getPic(&out, kControls[i].nullPic ? NULL : &pic, NULL);

        LOG("  %-24s -> 0x%08x  expect 0x%08x  %s\n",
            kControls[i].label, rc, kControls[i].expect,
            (uint32_t)rc == kControls[i].expect ? "ok" : "*** DIFFERS ***");
    }
}

/* ------------------------------------------------------------------------- */
/* Reporting one decode result                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    decode_fn           decode;
    get_picture_info_fn getPic;
    const dmem_block   *blocks;
    size_t              nBlocks;
    int                 pictures;      /* how many valid pictures so far */
    int                 dumpFrame;
    int                 frameDumped;
} decode_ctx;

static void
report_output(decode_ctx *ctx, const char *what, int rc,
              const SceVideodec2OutputInfo *out,
              const SceVideodec2FrameBuffer *fb)
{
    const char *where = NULL;
    size_t      avail;

    LOG("  %-22s rc=0x%08x %s\n", what, rc, vdec2_err((uint32_t)rc));
    LOG("    outputInfo raw:\n");
    hexdump(out, sizeof *out, "      ");
    LOG("    isValid %u  isErrorFrame %u  pictureCount %u  streamState %u\n",
        out->isValid, out->isErrorFrame, out->pictureCount, out->streamState);
    LOG("    codecType 0x%x %s  pts 0x%llx  word18 0x%x  word30 0x%x  word34 0x%x\n",
        out->codecType,
        out->codecType == CODEC_AVC  ? "(H.264)" :
        out->codecType == CODEC_HEVC ? "(HEVC)"  :
        out->codecType == CODEC_VP9  ? "(VP9)"   : "(none)",
        (unsigned long long)out->ptsData, out->word18, out->word30, out->word34);
    LOG("    frameBuffer %p  size %llu  (our buffer %p size %llu, isAccepted %u)\n",
        out->frameBuffer, (unsigned long long)out->frameBufferSize,
        fb ? fb->frameBuffer : NULL,
        (unsigned long long)(fb ? fb->frameBufferSize : 0),
        fb ? fb->isAccepted : 0);

    if (rc || !out->isValid)
        return;

    ctx->pictures++;
    LOG("\n    *** A PICTURE CAME BACK ***\n");

    avail = readable_from(ctx->blocks, ctx->nBlocks, out->frameBuffer, &where);
    LOG("    the frame pointer lands in: %s", where);
    if (avail)
        LOG(", %zu bytes readable from it\n", avail);
    else
        LOG("\n    NOT dereferencing it - see standing rule on speculative reads\n");

    if (!avail || out->frameBufferSize > avail)
        return;

    /* -- the picture metadata, where GetPictureInfo reads from ------------- */
    {
        size_t metaBytes = (size_t)out->pictureCount * PICTURE_META_SIZE;

        if (out->frameBufferSize <= metaBytes) {
            LOG("\n    reported buffer (%llu B) is not larger than %zu B of\n"
                "    metadata - not reading it, and GetPictureInfo will refuse\n"
                "    with 0x811D0106 for the same reason\n",
                (unsigned long long)out->frameBufferSize, metaBytes);
        } else {
            const uint8_t *meta = (const uint8_t *)out->frameBuffer +
                                  out->frameBufferSize - metaBytes;

            LOG("\n    picture metadata block at frameBuffer + %llu - %u*1024 = %p\n",
                (unsigned long long)out->frameBufferSize, out->pictureCount, meta);
            LOG("    first 0x80 bytes (GetPictureInfo's whole source):\n");
            hexdump(meta, 0x80, "      ");
        }
    }

    /* -- what the module makes of it -------------------------------------- */
    {
        SceVideodec2AvcPictureInfo pic0, pic1;
        int prc;

        memset(&pic0, 0, sizeof pic0);
        memset(&pic1, 0, sizeof pic1);
        pic0.thisSize = AVC_PICTURE_INFO_SIZE;
        pic1.thisSize = AVC_PICTURE_INFO_SIZE;

        prc = ctx->getPic(out, &pic0, &pic1);
        LOG("\n    sceVideodec2GetPictureInfo -> 0x%08x %s\n", prc,
            vdec2_err((uint32_t)prc));
        if (prc == 0) {
            LOG("    picture 0, raw 0x78 bytes (isValid %u):\n", pic0.isValid);
            hexdump(&pic0, sizeof pic0, "      ");
            if (out->pictureCount > 1) {
                LOG("    picture 1, raw 0x78 bytes (isValid %u):\n", pic1.isValid);
                hexdump(&pic1, sizeof pic1, "      ");
            }
        }
    }

    /* -- the first few bytes of each plane, for Phase 7 -------------------- */
    {
        const uint8_t *f = out->frameBuffer;
        uint64_t stride  = round_up(TEST_STREAM_WIDTH, 256);
        uint64_t rows    = round_up(TEST_STREAM_HEIGHT, 16);

        LOG("\n    frame bytes, on the assumption stride=%llu rows=%llu\n"
            "    (the offline arithmetic, NOT yet confirmed - Phase 7's job):\n",
            (unsigned long long)stride, (unsigned long long)rows);

        /* Sample only offsets the decoder's own reported size covers. The
         * prediction is what is being tested here; reading past the buffer
         * because the prediction was wrong would turn a wrong guess into a
         * dead payload. */
        if (out->frameBufferSize < stride * rows + 0x40) {
            LOG("      the reported buffer is %llu B, smaller than the predicted\n"
                "      luma plane plus a sample (%llu B). THE PREDICTION IS WRONG,\n"
                "      which is itself the answer - sampling only row 0.\n",
                (unsigned long long)out->frameBufferSize,
                (unsigned long long)(stride * rows + 0x40));
            hexdump(f, 0x40, "        ");
        } else {
            LOG("      luma row 0:\n");
            hexdump(f, 0x40, "        ");
            LOG("      luma row 1 (+%llu):\n", (unsigned long long)stride);
            hexdump(f + stride, 0x40, "        ");
            LOG("      luma row %llu (+%llu):\n",
                (unsigned long long)(rows / 2),
                (unsigned long long)(stride * (rows / 2)));
            hexdump(f + stride * (rows / 2), 0x40, "        ");
            LOG("      where chroma would start (+%llu):\n",
                (unsigned long long)(stride * rows));
            hexdump(f + stride * rows, 0x40, "        ");
        }
    }

    /* -- optionally, the whole frame, for offline inspection --------------- */
    if (ctx->dumpFrame && !ctx->frameDumped) {
        FILE *f = fopen(FRAME_PATH, "wb");

        if (!f) {
            LOG("\n    could not open %s for the frame dump\n", FRAME_PATH);
        } else {
            size_t n = fwrite(out->frameBuffer, 1,
                              (size_t)out->frameBufferSize, f);
            fclose(f);
            ctx->frameDumped = 1;
            LOG("\n    wrote %zu of %llu bytes to %s\n", n,
                (unsigned long long)out->frameBufferSize, FRAME_PATH);
            LOG("    Read it on the PC. That settles pixel format, stride and\n"
                "    plane layout offline, which is Phase 7's first question.\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* One framing attempt, self-contained                                       */
/* ------------------------------------------------------------------------- */

typedef enum { FRAMING_ANNEXB, FRAMING_AVCC } framing_t;

/* How many output buffers to register. The block table holds 16, which is also
 * the maximum DPB - the two limits matching is the reason to think the blocks
 * are the output frame buffers. Four is enough to let the decoder hold a
 * reference while writing the next picture, without asking for 16 x 3.3 MiB. */
#define MAP_BLOCKS 4

/* Every one of these is refused before the block table or the GpDec state is
 * touched, so a failed control leaves the decoder exactly as it was and the
 * real map that follows is unaffected. */
static void
map_controls(map_direct_memory_fn mapDirect, void *decoder,
             const SceVideodec2MapDirectMemoryInfo *good)
{
    static const struct {
        const char *label;
        uint64_t    thisSize;
        int         nullInfo, junkDecoder, zeroSize;
        uint32_t    expect;
    } kControls[] = {
        { "decoder = &junk",  0x20, 0, 1, 0, 0x811d0103 },
        { "info NULL",        0x20, 1, 0, 0, 0x811d0102 },
        { "info size 0x18",   0x18, 0, 0, 0, 0x811d0101 },
        { "size 0",           0x20, 0, 0, 1, 0x811d0104 },
    };
    uint64_t junk[16] = {0};

    LOG("\n--- MapDirectMemory: validation controls ---\n");

    for (size_t i = 0; i < sizeof kControls / sizeof *kControls; i++) {
        SceVideodec2MapDirectMemoryInfo info = *good;
        int rc;

        info.thisSize = kControls[i].thisSize;
        if (kControls[i].zeroSize)
            info.size = 0;

        if (kControls[i].nullInfo)
            rc = mapDirect(decoder, NULL);
        else if (kControls[i].junkDecoder)
            rc = mapDirect(junk, &info);
        else
            rc = mapDirect(decoder, &info);

        LOG("  %-22s -> 0x%08x  expect 0x%08x  %s\n",
            kControls[i].label, rc, kControls[i].expect,
            (uint32_t)rc == kControls[i].expect ? "ok" : "*** DIFFERS ***");
    }
}

/* Queries, allocates, creates, decodes, flushes, deletes, releases. Owns every
 * buffer it uses, so an attempt that fails leaves nothing behind for the next.
 * Returns the number of valid pictures the decoder reported. */
static int
attempt_decode(query_decoder_meminfo_fn query, create_decoder_fn create,
               delete_decoder_fn del, decode_fn decode, flush_fn flush,
               get_picture_info_fn getPic, map_direct_memory_fn mapDirect,
               const char *label, framing_t framing,
               uint32_t resourceType, uint32_t depth, unsigned auCount,
               void *queue, int controls, int dumpFrame)
{
    SceVideodec2DecoderConfigInfo cfg;
    SceVideodec2DecoderMemoryInfo mem;
    dmem_block  blocks[3 + MAP_BLOCKS] = {{0}};  /* work, pool, input, targets */
    dmem_block *work = &blocks[0], *pool = &blocks[1], *inbuf = &blocks[2];
    dmem_block *targets = &blocks[3];
    int         mapped = 0;
    uint64_t    mapLen = 0;
    int         inputMapped = 0;
    void       *mapAddr[MAP_BLOCKS] = {0};   /* what was actually registered */
    void       *dec = NULL;
    uint8_t    *avcc = NULL;
    decode_ctx  ctx;
    uint64_t    predicted;
    int         rc;

    memset(&ctx, 0, sizeof ctx);
    ctx.decode    = decode;
    ctx.getPic    = getPic;
    ctx.blocks    = blocks;
    ctx.nBlocks   = 3 + MAP_BLOCKS;
    ctx.dumpFrame = dumpFrame;

    LOG("\n===========================================================\n");
    LOG("=== decode attempt: %s\n", label);
    LOG("===========================================================\n");
    LOG("  resourceType 0x%x  pipelineDepth %u  framing %s  feeding %u AU%s\n",
        resourceType, depth,
        framing == FRAMING_ANNEXB ? "Annex-B (start codes)"
                                  : "AVCC (4-byte lengths)",
        auCount, auCount == 1 ? "" : "s");

    cfg_init(&cfg, resourceType, CODEC_AVC, TEST_STREAM_PROFILE,
             TEST_STREAM_LEVEL, TEST_STREAM_WIDTH, TEST_STREAM_HEIGHT, 16,
             depth, queue);

    memset(&mem, 0, sizeof mem);
    mem.thisSize = DECODER_MEMINFO_SIZE;

    rc = query(&cfg, &mem);
    LOG("  QueryDecoderMemoryInfo -> 0x%08x %s\n", rc, vdec2_err((uint32_t)rc));
    if (rc)
        return 0;

    predicted = predict_map_memory_size(CODEC_AVC, TEST_STREAM_PROFILE,
                                        TEST_STREAM_WIDTH, TEST_STREAM_HEIGHT);
    LOG("    work %llu  frame pool %llu  extra %llu  map %llu\n",
        (unsigned long long)mem.workMemorySize,
        (unsigned long long)mem.frameMemorySize,
        (unsigned long long)mem.extraMemorySize,
        (unsigned long long)mem.mapMemorySize);
    LOG("    mapMemorySize predicted %llu, measured %llu  %s\n",
        (unsigned long long)predicted,
        (unsigned long long)mem.mapMemorySize,
        predicted == mem.mapMemorySize ? "MATCH - the frame arithmetic holds"
                                       : "*** the frame arithmetic is WRONG ***");

    /* -- buffers ---------------------------------------------------------- */
    rc = dmem_get(work, mem.workMemorySize, SCE_KERNEL_WB_ONION);
    LOG("  work   %10llu B WB_ONION  -> 0x%08x virt %p\n",
        (unsigned long long)mem.workMemorySize, rc, work->vaddr);
    if (rc)
        goto out;

    rc = dmem_get(pool, mem.frameMemorySize, SCE_KERNEL_WC_GARLIC);
    LOG("  pool   %10llu B WC_GARLIC -> 0x%08x virt %p\n",
        (unsigned long long)mem.frameMemorySize, rc, pool->vaddr);
    if (rc)
        goto out;

    /* The output frame buffers. Each is mapMemorySize, which is exactly one
     * output frame plus its five metadata slots. MAP_BLOCKS of them, because
     * the block table holds 16 and the decoder may hold one as a reference
     * while writing the next. */
    for (int i = 0; i < MAP_BLOCKS; i++) {
        rc = dmem_get(&targets[i], mem.mapMemorySize, SCE_KERNEL_WC_GARLIC);
        LOG("  frame%d %10llu B WC_GARLIC -> 0x%08x virt %p phys 0x%lx  "
            "(256-aligned: %s)\n",
            i, (unsigned long long)mem.mapMemorySize, rc, targets[i].vaddr,
            (unsigned long)targets[i].paddr,
            ((uintptr_t)targets[i].vaddr & 0xff) ? "NO - Decode will refuse"
                                                 : "yes");
        if (rc)
            goto out;
        memset(targets[i].vaddr, 0, targets[i].len);
    }

    /* The input buffer.
     *
     * THE ACCESS UNIT HAS TO LIVE IN MAPPED DIRECT MEMORY TOO. Run 5 got past
     * the state check with four output blocks registered and then failed at
     * GpDec line 0xDA6, which is one arm of a loop that walks the registered
     * blocks asking whether each region the decode input names is *contained*
     * in one of them. The bitstream is DMA'd by the hardware, so pointing
     * auData at the payload's own .rodata - where .incbin put the stream -
     * could never have worked. It has to be copied into memory the decoder has
     * been told about. */
    rc = dmem_get(inbuf, MiB(1), SCE_KERNEL_WB_ONION);
    LOG("  input  %10llu B WB_ONION  -> 0x%08x virt %p phys 0x%lx\n",
        (unsigned long long)MiB(1), rc, inbuf->vaddr,
        (unsigned long)inbuf->paddr);
    if (rc)
        goto out;
    memset(inbuf->vaddr, 0, inbuf->len);

    memset(work->vaddr, 0, DECODER_OBJECT_SIZE);

    mem.pWorkMemory  = work->vaddr;
    mem.pFrameMemory = pool->vaddr;
    mem.pExtraMemory = NULL;

    /* -- the decoder ------------------------------------------------------ */
    rc = create(&cfg, &mem, &dec);
    LOG("\n  sceVideodec2CreateDecoder -> 0x%08x %s decoder=%p\n", rc,
        vdec2_err((uint32_t)rc), dec);
    if (rc || !dec)
        goto out;

    {
        uint64_t cookie;

        memcpy(&cookie, (const uint8_t *)dec + 0x68, sizeof cookie);
        LOG("    +0x68 cookie 0x%016llx %s\n", (unsigned long long)cookie,
            cookie == DECODER_MAGIC1 ? "as read" : "*** UNEXPECTED ***");
    }

    /* -- register the output buffers -------------------------------------- *
     * THE STEP THE FIRST RUN WAS MISSING. Until sceVdecCoreMapMemoryBlock has
     * run, the GpDec object sits in state 0 and refuses every access unit with
     * the 0x80C00001 that Videodec2 reports as 0x811D0111. Mapping is legal
     * only while that state is below 2, so it happens here - after Create,
     * before the first Decode - and never again. */
    {
        SceVideodec2MapDirectMemoryInfo info;
        int winner = -1;

        memset(&info, 0, sizeof info);
        info.thisSize = 0x20;
        info.size     = mem.mapMemorySize;
        info.addr     = targets[0].vaddr;

        if (controls)
            map_controls(mapDirect, dec, &info);

        /* -- both fields, finally right at the same time ------------------ *
         * This took three runs because the two unknowns were never correct
         * together:
         *
         *   run 2  size = mapMemorySize (wrong), physAddr = paddr then 0.
         *          Both refused, and the physAddr half was inconclusive BY
         *          CONSTRUCTION - with mode 0 the field never reaches the
         *          driver ioctl, so both attempts issued identical calls.
         *   run 3  size laddered, physAddr = 0. Block 0 was accepted at the
         *          16 KiB rounding; block 1 was refused at every length.
         *   run 4  block 1's refusal is GpDec line 0x97F, status 0x33 - an
         *          OVERLAP check, and not the one over CPU addresses.
         *
         * The 0x97F loop compares [info+0x18, info+0x18 + size) against that
         * same range for every block already registered with mode 0:
         *
         *     rdi = slot+0x08          ; that block's info+0x18
         *     r9  = slot+0x10          ; that block's size
         *     add r9, rdi              ; its end
         *     cmp rcx, r9  ; setae     ; ours starts at or after theirs ends
         *     cmp rdx, rdi ; setbe     ; ours ends at or before theirs starts
         *     or  ; jne next           ; disjoint, keep looking
         *                              ; otherwise -> overlap, status 0x33
         *
         * So info+0x18 IS AN ADDRESS, in a space of its own, and setting it to
         * zero for every block made them all occupy [0, size) and collide.
         * The direct-memory physical offset is what makes each block distinct
         * - which is what "DirectMemory" in the name was saying all along.
         *
         * Length: 16 KiB rounded. Established by run 3's ladder. The raw
         * mapMemorySize (0x331400) is not a multiple of any page size, and the
         * call bottoms out in a pin request,
         * ioctl(fd, _IOW(0x83, 20, 40), {base, len, 0, 0, 1}). 4 KiB rounding
         * was still refused; 16 KiB was taken.
         */
        {
            uint64_t mapSize = round_up(mem.mapMemorySize, 0x4000);

            LOG("\n--- MapDirectMemory: registering %d output buffers ---\n",
                MAP_BLOCKS);
            LOG("    size %llu (mapMemorySize %llu rounded up to 16 KiB)\n"
                "    info+0x18 = the direct-memory physical offset, which is\n"
                "    what keeps the blocks from colliding in the second space.\n\n",
                (unsigned long long)mapSize,
                (unsigned long long)mem.mapMemorySize);

            for (int i = 0; i < MAP_BLOCKS; i++) {
                info.addr     = targets[i].vaddr;
                info.size     = mapSize;
                info.physAddr = (uint64_t)targets[i].paddr;

                rc = mapDirect(dec, &info);
                LOG("  block %d  addr %p phys 0x%09llx len %llu -> 0x%08x %s\n",
                    i, info.addr, (unsigned long long)info.physAddr,
                    (unsigned long long)info.size, rc,
                    rc ? vdec2_err((uint32_t)rc) : "ACCEPTED");
                if (rc)
                    break;
                mapAddr[mapped++] = info.addr;
                mapLen = info.size;
            }

            /* Control for the reading above: a CPU address that has NOT been
             * registered, carrying block 0's physical offset. The CPU-address
             * loop cannot object to it. If it is refused anyway, info+0x18 is
             * what the second overlap check uses, and the diagnosis is right
             * rather than merely plausible. */
            if (mapped) {
                info.addr     = pool->vaddr;
                info.size     = mapSize;
                info.physAddr = (uint64_t)targets[0].paddr;

                rc = mapDirect(dec, &info);
                LOG("\n  control: unregistered CPU address carrying block 0's\n"
                    "           physical offset -> 0x%08x  %s\n", rc,
                    rc ? "refused, as predicted"
                       : "*** ACCEPTED - the reading above is WRONG ***");
            }

            /* And everything else the decoder will DMA.
             *
             * GpDec's line 0xDA6 walks the registered blocks asking whether
             * each of three regions is CONTAINED in one of them, and refuses
             * if not:
             *
             *     region A   [gpdec+0xda0], size [gpdec+0xd98]
             *     region B   [gpdec+0xdb8], size [gpdec+0xdb0]   <- 0xDA6
             *     region C   the access unit descriptor
             *
             * A and B are the buffers handed to sceVdecCoreCreateDecoder -
             * the work memory past the 0x40000 decoder object, and the frame
             * pool. Giving CreateDecoder a pointer is not the same as telling
             * the hardware about it: they have to be registered here as well.
             * That is what run 6 was missing after the input buffer was fixed
             * and the failure stayed at 0xDA6. */
            {
                const struct {
                    const char *label;
                    dmem_block *blk;
                } kAlso[] = {
                    { "input", inbuf },
                    { "work",  work  },
                    { "pool",  pool  },
                };

                for (size_t k = 0; k < sizeof kAlso / sizeof *kAlso; k++) {
                    info.addr     = kAlso[k].blk->vaddr;
                    info.size     = kAlso[k].blk->len;
                    info.physAddr = (uint64_t)kAlso[k].blk->paddr;

                    rc = mapDirect(dec, &info);
                    LOG("  %-6s addr %p phys 0x%09llx len %10llu -> 0x%08x %s\n",
                        kAlso[k].label, info.addr,
                        (unsigned long long)info.physAddr,
                        (unsigned long long)info.size, rc,
                        rc ? vdec2_err((uint32_t)rc) : "ACCEPTED");
                    if (k == 0)
                        inputMapped = (rc == 0);
                }
            }

            LOG("\n  %d of %d output buffers registered, input %s\n", mapped,
                MAP_BLOCKS, inputMapped ? "registered" : "NOT REGISTERED");
        }

        if (!mapped) {
            LOG("\n  NO BUFFER REGISTERED - Decode will refuse exactly as it\n"
                "  did before. Stopping rather than re-measuring a failure\n"
                "  that is already recorded.\n");
            goto out;
        }
        (void)winner;
    }

    /* -- the access unit -------------------------------------------------- */
    {
        SceVideodec2InputData   au;
        SceVideodec2FrameBuffer fb;

        memset(&au, 0, sizeof au);
        au.thisSize = INPUT_DATA_SIZE;
        memcpy(inbuf->vaddr, test_stream_h264 + kTestAUs[0].offset,
               kTestAUs[0].length);
        au.auData   = inbuf->vaddr;
        au.auSize   = kTestAUs[0].length;

        memset(&fb, 0, sizeof fb);
        fb.thisSize        = FRAME_BUFFER_SIZE;
        fb.frameBuffer     = mapAddr[0];
        fb.frameBufferSize = mapLen;

        if (controls) {
            decode_controls(decode, dec, &au, &fb);
            picture_info_controls(getPic, mapAddr[0]);
        }

        /* -- and now the hardware ----------------------------------------- */
        LOG("\n--- feeding the decoder ---\n");
        LOG("    Everything above this line is already in the log. If the run\n"
            "    stops here, sceVdecCoreSyncDecode blocked on the hardware and\n"
            "    that is itself the finding.\n\n");

        if (framing == FRAMING_AVCC) {
            avcc = malloc(test_stream_h264_len + 64);
            if (!avcc) {
                LOG("    could not allocate the AVCC scratch buffer\n");
                goto out;
            }
        }

        for (unsigned i = 0; i < auCount && i < TEST_STREAM_AU_COUNT; i++) {
            SceVideodec2OutputInfo out;
            const uint8_t         *src = test_stream_h264 + kTestAUs[i].offset;
            size_t                 len = kTestAUs[i].length;

            if (framing == FRAMING_AVCC) {
                len = annexb_to_avcc(src, len, avcc,
                                     test_stream_h264_len + 64);
                if (!len) {
                    LOG("    AU %u: could not be re-framed as AVCC\n", i);
                    continue;
                }
                src = avcc;
            }

            /* Into the registered input block, never straight from .rodata. */
            if (len > inbuf->len) {
                LOG("    AU %u is %zu bytes, larger than the input block\n",
                    i, len);
                break;
            }
            memcpy(inbuf->vaddr, src, len);
            au.auData       = inbuf->vaddr;
            au.auSize       = len;
            au.ptsData      = 1000 + i;          /* recognisable, ascending  */
            au.dtsData      = 2000 + i;
            au.attachedData = 0xE0000000ull + i; /* does it round-trip?      */

            memset(&out, 0, sizeof out);
            out.thisSize = OUTPUT_INFO_SIZE;
            fb.isAccepted = 0;

            /* Round-robin the registered buffers, so the decoder is never
             * handed back one it may still be holding as a reference. */
            fb.frameBuffer = mapAddr[i % (unsigned)mapped];

            LOG("\n  --- AU %u: %zu bytes, %s, pts %llu, into block %u ---\n",
                i, len, kTestAUs[i].isIdr ? "IDR" : "P",
                (unsigned long long)au.ptsData, i % (unsigned)mapped);

            rc = decode(dec, &au, &fb, &out);

            {
                char what[48];
                snprintf(what, sizeof what, "Decode(AU %u)", i);
                report_output(&ctx, what, rc, &out, &fb);
            }

            if (rc)
                break;
        }

        /* -- drain -------------------------------------------------------- */
        LOG("\n--- flush: draining whatever the pipeline still holds ---\n");
        LOG("    Flush latches decoder+0x50, after which every Decode returns\n"
            "    0x811D0100 until Reset. It goes last for that reason.\n");
        for (int i = 0; i < 8; i++) {
            SceVideodec2OutputInfo out;
            char what[48];

            memset(&out, 0, sizeof out);
            out.thisSize = OUTPUT_INFO_SIZE;
            fb.isAccepted = 0;

            rc = flush(dec, &fb, &out);
            snprintf(what, sizeof what, "Flush(%d)", i);
            report_output(&ctx, what, rc, &out, &fb);

            if (rc || !out.isValid)
                break;
        }
    }

out:
    if (dec) {
        rc = del(dec);
        LOG("\n  sceVideodec2DeleteDecoder -> 0x%08x %s\n", rc,
            vdec2_err((uint32_t)rc));
    }
    free(avcc);
    for (int i = 0; i < MAP_BLOCKS; i++)
        dmem_put(&targets[i]);
    dmem_put(inbuf);
    dmem_put(pool);
    dmem_put(work);
    LOG("  buffers released; %d picture%s reported by this attempt\n",
        ctx.pictures, ctx.pictures == 1 ? "" : "s");

    return ctx.pictures;
}

/* ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
    unsigned auCount   = 4;
    int      dumpFrame = 0;
    int      tryAvcc   = 1;
    int      useArb    = 1;
    int      arbUp     = 0;
    int      have_control = 0, queue_ok = 0;
    int      annexbPics = 0, avccPics = 0;
    size_t   compute_size;

    dmem_block cq_mem = {0};
    void      *queue  = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], "dump") == 0)
            dumpFrame = 1;
        else if (strcmp(argv[i], "one") == 0)
            auCount = 1;
        else if (strcmp(argv[i], "all") == 0)
            auCount = TEST_STREAM_AU_COUNT;
        else if (strcmp(argv[i], "no-avcc") == 0)
            tryAvcc = 0;
        else if (strcmp(argv[i], "no-arb") == 0)
            useArb = 0;
    }

    g_log = fopen(LOG_PATH, "w");

    LOG("=== EVO Player decodeframe_test - Phase 6 ===\n");
    LOG("firmware : 0x%08x\n", kernel_get_fw_version());
    LOG("pid      : %d\n", getpid());
    LOG("stream   : %u bytes, %u access units, %ux%u High L4.0\n",
        test_stream_h264_len, TEST_STREAM_AU_COUNT,
        TEST_STREAM_WIDTH, TEST_STREAM_HEIGHT);
    LOG("feeding  : %u AU%s per attempt\n", auCount, auCount == 1 ? "" : "s");
    LOG("frame dump: %s   AVCC retry: %s\n",
        dumpFrame ? "on (pass \"dump\")" : "off - pass \"dump\" to enable",
        tryAvcc ? "on" : "off");

    /* The first bytes of AU 0, so the log proves what was fed rather than
     * describing it. 00 00 00 01 09 is an access unit delimiter. */
    LOG("\nAU 0, first 0x30 bytes as linked into this payload:\n");
    hexdump(test_stream_h264 + kTestAUs[0].offset, 0x30, "  ");

    for (size_t i = 0; i < sizeof kModules / sizeof *kModules; i++) {
        char path[256];
        int  res = 0;
        int  modid;

        snprintf(path, sizeof path, "/system/common/lib/%s", kModules[i]);
        modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        LOG("load %-38s modid=0x%x res=0x%x\n", kModules[i], modid, res);
    }

    /* Arbitration first: AvPlayer initialises it before it touches a decoder,
     * and if it is what the submit is waiting on, everything below behaves
     * differently. Cheap, reversible, and every failure is an error return. */
    if (useArb) {
        uint32_t dynh_arb = 0;

        if (kernel_dynlib_handle(getpid(), "libSceVideoDecoderArbitration.sprx",
                                 &dynh_arb) == 0) {
            intptr_t arb_base = kernel_dynlib_mapbase_addr(getpid(), dynh_arb);

            LOG("\nlibSceVideoDecoderArbitration base 0x%lx\n",
                (unsigned long)arb_base);
            arbUp = arbitration_bring_up(dynh_arb, arb_base);
        } else {
            LOG("\nno dynlib handle for libSceVideoDecoderArbitration.sprx\n");
        }
    } else {
        LOG("\narbitration skipped (no-arb)\n");
    }

    uint32_t dynh = 0;
    if (kernel_dynlib_handle(getpid(), "libSceVideodec2.sprx", &dynh) != 0) {
        LOG("\nFATAL: no dynlib handle for libSceVideodec2.sprx\n");
        return EXIT_FAILURE;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(getpid(), dynh);
    LOG("\nlibSceVideodec2 base 0x%lx\n", (unsigned long)base);

    intptr_t a_query    = resolve(dynh, base, "sceVideodec2QueryDecoderMemoryInfo", 0xa10);
    intptr_t a_create   = resolve(dynh, base, "sceVideodec2CreateDecoder", 0xba0);
    intptr_t a_delete   = resolve(dynh, base, "sceVideodec2DeleteDecoder", 0x3220);
    intptr_t a_decode   = resolve(dynh, base, "sceVideodec2Decode", 0x1290);
    intptr_t a_flush    = resolve(dynh, base, "sceVideodec2Flush", 0x1bf0);
    intptr_t a_reset    = resolve(dynh, base, "sceVideodec2Reset", 0x30f0);
    intptr_t a_getpic   = resolve(dynh, base, "sceVideodec2GetPictureInfo", 0x2120);
    intptr_t a_mapdirect = resolve(dynh, base, "sceVideodec2MapDirectMemory", 0x340);
    intptr_t a_query_cm = resolve(dynh, base, "sceVideodec2QueryComputeMemoryInfo", 0x550);
    intptr_t a_alloc_cq = resolve(dynh, base, "sceVideodec2AllocateComputeQueue", 0x660);
    intptr_t a_rel_cq   = resolve(dynh, base, "sceVideodec2ReleaseComputeQueue", 0x970);

    /* -- CONTROL: the alias map, free ------------------------------------- *
     * Four of the eighteen exports are two pairs of byte-identical thunks
     * into one body. Resolving them records the aliasing instead of asserting
     * it, and standing rule 14 exists because Phase 5 designed an experiment
     * around an entry point that turned out to be one of these. */
    LOG("\n  the picture-info entry points - all four reach the body at +0x2130:\n");
    intptr_t a_getavc  = resolve(dynh, base, "sceVideodec2GetAvcPictureInfo", 0x30c0);
    intptr_t a_gethevc = resolve(dynh, base, "sceVideodec2GetHevcPictureInfo", 0x30e0);
    intptr_t a_getvp9  = resolve(dynh, base, "sceVideodec2GetVp9PictureInfo", 0x30d0);
    LOG("    GetPictureInfo and GetAvcPictureInfo are byte-identical thunks;\n"
        "    so are GetHevcPictureInfo and GetVp9PictureInfo. The body\n"
        "    dispatches on outputInfo->codecType, not on the name called.\n");
    (void)a_getavc; (void)a_gethevc; (void)a_getvp9; (void)a_reset;

    /* Resolved and deliberately NOT called: MapMemory's one import is unbound
     * in the dump, which is the shape of a silent indefinite block. Logging its
     * offset records that it was considered and rejected. */
    (void)resolve(dynh, base, "sceVideodec2MapMemory", 0xe0);
    LOG("    ^ resolved but never called - its GOT slot (s2 +0x60) is unbound.\n"
        "      MapDirectMemory (+0x340) is the bound route to the same layer.\n");

    if (!a_query || !a_create || !a_delete || !a_decode || !a_flush ||
        !a_getpic || !a_mapdirect || !a_query_cm || !a_alloc_cq || !a_rel_cq) {
        LOG("\nFATAL: an entry point did not resolve\n");
        return EXIT_FAILURE;
    }

    query_decoder_meminfo_fn query    = (query_decoder_meminfo_fn)a_query;
    create_decoder_fn        create   = (create_decoder_fn)a_create;
    delete_decoder_fn        del      = (delete_decoder_fn)a_delete;
    decode_fn                decode   = (decode_fn)a_decode;
    flush_fn                 flush    = (flush_fn)a_flush;
    get_picture_info_fn      getPic   = (get_picture_info_fn)a_getpic;
    map_direct_memory_fn     mapDirect = (map_direct_memory_fn)a_mapdirect;
    query_compute_meminfo_fn query_cm = (query_compute_meminfo_fn)a_query_cm;
    alloc_compute_queue_fn   alloc_cq = (alloc_compute_queue_fn)a_alloc_cq;
    release_compute_queue_fn rel_cq   = (release_compute_queue_fn)a_rel_cq;

    /* -- CONTROL: Phase 5's query, re-measured ---------------------------- */
    LOG("\n--- control: the Phase 5 query, AVC 1080p dpb16 depth4 ---\n");
    {
        SceVideodec2DecoderConfigInfo cfg;
        SceVideodec2DecoderMemoryInfo mem;
        int rc;

        cfg_init(&cfg, RES_STD, CODEC_AVC, 100, 51, 1920, 1080, 16, 4, NULL);
        memset(&mem, 0, sizeof mem);
        mem.thisSize = DECODER_MEMINFO_SIZE;

        rc = query(&cfg, &mem);
        have_control = (rc == 0 && mem.frameMemorySize == PHASE5_1080P_FRAMEPOOL);
        LOG("  rc=0x%08x  frame pool %llu  (Phase 5: %llu)  %s\n", rc,
            (unsigned long long)mem.frameMemorySize,
            (unsigned long long)PHASE5_1080P_FRAMEPOOL,
            have_control ? "MATCH" : "*** DIVERGED ***");
    }

    /* -- CONTROL: the frame-size arithmetic, against the module ----------- *
     * Free, and it is the whole of Phase 7's first question answered ahead
     * of time - if it holds. A mismatch here is far better found now than
     * after a frame has been misread. */
    LOG("\n--- control: mapMemorySize predicted vs measured ---\n");
    LOG("    predicted = align(w,256) * align(h,N) * bytes * 3/2 + 5*1024\n"
        "    with N = 16 for H.264 and 1 for HEVC, bytes = 2 for Main10.\n"
        "    If every row matches, the decoder's output is NV12 or P010 at a\n"
        "    256-aligned stride and Phase 7 starts from a known layout.\n\n");
    LOG("  %-34s %12s %12s  %s\n", "configuration", "predicted", "measured", "");
    {
        static const struct {
            const char *label;
            uint32_t    res, codec, profile, level, w, h;
        } kCases[] = {
            { "AVC  High   L4.0  1280x720 ", RES_STD, CODEC_AVC,  100,  40, 1280,  720 },
            { "AVC  High   L5.1  1920x1080", RES_STD, CODEC_AVC,  100,  51, 1920, 1080 },
            { "AVC  High   L5.1  3840x2160", RES_BIG, CODEC_AVC,  100,  51, 3840, 2160 },
            { "HEVC Main   L4.0  1920x1080", RES_BIG, CODEC_HEVC,   1, 120, 1920, 1080 },
            { "HEVC Main10 L4.0  1920x1080", RES_BIG, CODEC_HEVC,   2, 120, 1920, 1080 },
            { "HEVC Main   L5.1  3840x2160", RES_BIG, CODEC_HEVC,   1, 153, 3840, 2160 },
            { "HEVC Main10 L5.1  3840x2160", RES_BIG, CODEC_HEVC,   2, 153, 3840, 2160 },
        };

        for (size_t i = 0; i < sizeof kCases / sizeof *kCases; i++) {
            SceVideodec2DecoderConfigInfo cfg;
            SceVideodec2DecoderMemoryInfo mem;
            uint64_t predicted;
            int rc;

            cfg_init(&cfg, kCases[i].res, kCases[i].codec, kCases[i].profile,
                     kCases[i].level, kCases[i].w, kCases[i].h, 16, 4, NULL);
            memset(&mem, 0, sizeof mem);
            mem.thisSize = DECODER_MEMINFO_SIZE;

            rc = query(&cfg, &mem);
            predicted = predict_map_memory_size(kCases[i].codec,
                                                kCases[i].profile,
                                                kCases[i].w, kCases[i].h);
            if (rc)
                LOG("  %-34s %12llu %12s  0x%08x %s\n", kCases[i].label,
                    (unsigned long long)predicted, "-", rc,
                    vdec2_err((uint32_t)rc));
            else
                LOG("  %-34s %12llu %12llu  %s\n", kCases[i].label,
                    (unsigned long long)predicted,
                    (unsigned long long)mem.mapMemorySize,
                    predicted == mem.mapMemorySize ? "MATCH" : "*** DIFFERS ***");
        }
    }

    /* -- the compute queue, exactly as Phase 4 proved it ------------------ */
    LOG("\n--- compute queue ---\n");
    {
        SceVideodec2ComputeMemoryInfo mi;
        SceVideodec2ComputeQueueInfo  qi;
        int rc;

        memset(&mi, 0, sizeof mi);
        mi.thisSize = COMPUTE_MEMINFO_SIZE;
        rc = query_cm(&mi);
        LOG("  QueryComputeMemoryInfo -> 0x%08x  size %llu (%.2f MiB)\n", rc,
            (unsigned long long)mi.cpuGpuMemorySize,
            (double)mi.cpuGpuMemorySize / (1024.0 * 1024.0));
        if (rc)
            goto done;

        compute_size = (size_t)mi.cpuGpuMemorySize;
        rc = dmem_get(&cq_mem, compute_size, SCE_KERNEL_WB_ONION);
        LOG("  queue memory %zu B WB_ONION -> 0x%08x virt %p\n",
            compute_size, rc, cq_mem.vaddr);
        if (rc)
            goto done;
        memset(cq_mem.vaddr, 0, cq_mem.len);

        memset(&qi, 0, sizeof qi);
        qi.thisSize = COMPUTE_QUEUEINFO_SIZE;
        mi.cpuGpuMemorySize = compute_size;
        mi.cpuGpuMemory     = cq_mem.vaddr;

        rc = alloc_cq(&qi, &mi, &queue);
        LOG("  AllocateComputeQueue(pipe 0, queue 0) -> 0x%08x handle %p\n",
            rc, queue);
        queue_ok = (rc == 0 && queue != NULL);
        if (!queue_ok) {
            LOG("  no compute queue - stopping rather than passing NULL into a\n"
                "  path VdecCore does not NULL-check.\n");
            goto done;
        }
    }

    /* -- Annex-B, pipeline depth 1 ---------------------------------------- *
     * Depth 1 rather than Phase 5's 4 because the goal of this phase is the
     * shortest path from one access unit to one picture. The Phase 5 control
     * above still runs at depth 4, so the comparison with Phase 5's recorded
     * figures is unaffected. */
    annexbPics = attempt_decode(query, create, del, decode, flush, getPic,
                                mapDirect,
                                "Annex-B framing, depth 1, class 0xb6c8",
                                FRAMING_ANNEXB, RES_STD, 1, auCount, queue,
                                /*controls=*/1, dumpFrame);

    /* -- AVCC, only if Annex-B produced nothing --------------------------- */
    if (annexbPics == 0 && tryAvcc) {
        LOG("\n\nAnnex-B produced no picture. Re-framing the same access units\n"
            "as AVCC - 4-byte big-endian NAL lengths instead of start codes -\n"
            "on a fresh decoder. Nothing on the decode path scans for start\n"
            "codes, so which framing the hardware wants could not be settled\n"
            "offline; this is the experiment that settles it.\n");
        avccPics = attempt_decode(query, create, del, decode, flush, getPic,
                                  mapDirect,
                                  "AVCC framing, depth 1, class 0xb6c8",
                                  FRAMING_AVCC, RES_STD, 1, auCount, queue,
                                  /*controls=*/0, dumpFrame);
    } else if (annexbPics) {
        LOG("\nAVCC retry skipped: Annex-B already produced %d picture%s.\n",
            annexbPics, annexbPics == 1 ? "" : "s");
    }

done:
    if (queue) {
        int rc = rel_cq(queue);
        LOG("\nReleaseComputeQueue -> 0x%08x\n", rc);
    }
    dmem_put(&cq_mem);

    LOG("\n=== summary ===\n");
    LOG("  Phase 5 control re-measured : %s\n",
        have_control ? "MATCH" : "DIVERGED - distrust everything above");
    LOG("  compute queue               : %s\n",
        queue_ok ? "allocated" : "NOT OBTAINED");
    LOG("  arbitration                 : %s\n",
        !useArb ? "skipped" : arbUp ? "initialised and enabled" : "NOT UP");
    LOG("  pictures, Annex-B framing   : %d\n", annexbPics);
    if (annexbPics == 0 && tryAvcc)
        LOG("  pictures, AVCC framing      : %d\n", avccPics);
    else
        LOG("  pictures, AVCC framing      : not attempted\n");

    if (annexbPics || avccPics) {
        LOG("\n  *** PHASE 6 GOAL MET: the decoder reported a picture ***\n");
        LOG("  Next, Phase 7: re-run with \"dump\" to write a whole frame to\n"
            "  %s and read it on the PC. The offline\n"
            "  arithmetic says NV12 at a %llu-byte stride; that is a prediction\n"
            "  to check against real pixels, not a measurement.\n",
            FRAME_PATH, (unsigned long long)round_up(TEST_STREAM_WIDTH, 256));
    } else {
        LOG("\n  Reading the refusal:\n"
            "    0x811D0101/0102/0106/0107/0108/010D/010E -> argument shape.\n"
            "               Compare against the controls above; if a control\n"
            "               that should have failed did not, the struct\n"
            "               layout read is wrong.\n"
            "    0x811D0300/0304 -> the decoder latched an error state. That\n"
            "               is the layer below rejecting the bitstream, which\n"
            "               points at framing rather than at the API.\n"
            "    0x811D0100 after a Flush -> expected; the flush latch at\n"
            "               decoder+0x50 is one-way until Reset.\n"
            "    rc=0 with isValid 0 on every AU and every Flush -> the\n"
            "               decoder accepted the data and produced nothing.\n"
            "               That is a framing or a compute-queue problem, not\n"
            "               an entitlement one.\n"
            "    log stops inside an AU -> sceVdecCoreSyncDecode blocked on\n"
            "               the hardware. Where it stopped is the finding.\n");
    }

    LOG("\ndone\n");
    if (g_log)
        fclose(g_log);

    evo_notify("EVO decodeframe_test: %d picture(s) decoded",
               annexbPics + avccPics);
    return EXIT_SUCCESS;
}
