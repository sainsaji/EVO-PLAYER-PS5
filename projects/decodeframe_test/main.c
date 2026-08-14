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

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* Load order matters, and it was wrong. Two separate mistakes, both of the
 * shape standing rule 11 exists for - "load every module in the call chain":
 *
 * 1. THE H.264 CODEC MODULE WAS NEVER LOADED. Every run so far loaded
 *    libSceVdecShevc.sprx - the HEVC codec - and then decoded an H.264
 *    stream. /system/common/lib has libSceVdecSavc.sprx and
 *    libSceVdecSavc2.sprx sitting right next to it, and neither has ever been
 *    loaded by this project. codecdump_test measured that CreateDecoder loads
 *    NO module on the H.264 path, and that was recorded as "either the codec
 *    is already resident or that path is not taken". The third reading was
 *    never considered: the codec module is simply absent, CreateDecoder does
 *    not care because it only builds the object, and the absence first bites
 *    at the hardware submit - which is exactly where errno 5200 appears. The
 *    job VdecCore hands the driver is a POINTER TO A COMMAND BUFFER whose
 *    contents the codec layer builds; with no codec layer there is nothing
 *    sane in it to submit.
 *
 * 2. ARBITRATION WAS LOADED BEFORE ITS OWN BACKEND. libSceVideoArbitration.sprx
 *    is a DIFFERENT module from libSceVideoDecoderArbitration.sprx, it exists
 *    on this console, and it has never been loaded. See the GOT check in
 *    main() for why that is almost certainly the cause of the Initialize hang.
 *    Binding on this loader is eager at load time, so the provider has to be
 *    resident BEFORE the consumer is loaded - hence the position here.
 *
 * The other arbitration candidates are loaded too. They are cheap, and if the
 * GOT slot binds, the log says which module bound it. */
static const char *const kModules[] = {
    /* -- candidate providers, loaded FIRST so eager binding can see them -- */
    "libSceIpmi.sprx",              /* Sony's IPC layer for system services  */
    "libSceVideoArbitration.sprx",  /* NOT the same module as the one below  */
    "libSceResourceArbitrator.sprx",

    "libSceVdecCore.sprx",
    "libSceVideoDecoderArbitration.sprx",
    "libSceVideodec2.sprx",
    "libSceVdecwrap.sprx",
    "libSceVdecShevc.sprx",   /* HEVC codec - loaded since Phase 5           */
    "libSceGnmDriver.sprx",   /* sceGnmMapComputeQueue lives here            */
    "libSceSysmodule.sprx",   /* VdecCore loads its codec module through it  */
    "libSceAjm.sprx",

    /* -- the H.264 codec modules. THE STREAM IN THIS PAYLOAD IS H.264. ---- *
     * Loaded after GnmDriver and Ajm on purpose: libSceAudiodec is on record
     * as hanging when loaded before those two, and a codec module is exactly
     * the kind of thing that might share that dependency. */
    "libSceVdecSavc.sprx",
    "libSceVdecSavc2.sprx",
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

/* -- WHY Initialize HANGS, found offline, for free -------------------------
 *
 * findings.md called the hang "the body at +0x350 blocks", and hypothesised an
 * IPC to a system service. Both readings were wrong, and the disassembly says
 * so in three instructions:
 *
 *     800a3c350:  jmp QWORD PTR [rip+0x7cf2]   # 0x800a44048
 *     800a3c356:  push 0x1
 *     800a3c35b:  jmp  0x800a3c330
 *
 * +0x350 IS NOT A FUNCTION BODY. It is a PLT stub. Initialize validates its
 * parameters and then tail-calls an IMPORT - and in the Phase 0 dump that
 * import's GOT slot, segment 2 +0x48, still holds 0x800a3c356: the address of
 * its own push/jmp resolver sequence. It is UNBOUND, while its neighbours in
 * the same table are bound to libkernel and libSceLibcInternal.
 *
 * That is not a new failure mode. It is the THIRD time in this project:
 * sceVideodec2AllocateComputeQueue hung for exactly this reason until
 * libSceGnmDriver was loaded, and sceVideodec2MapMemory's slot is unbound in
 * the same way. Standing rule 15 - "check the GOT slot before you call the
 * function" - was written after the second one and simply was not applied to
 * arbitration. An unresolved lazy import blocks silently and forever: no
 * fault, no error code, no log line. Which is precisely what run 9 and run 10
 * observed, and why "no user session" looked plausible enough to spend a
 * deploy on.
 *
 * So this reads the slot BEFORE calling anything. If it is still the stub,
 * Initialize is not called at all - the hang is predicted rather than
 * suffered, and the deploy keeps everything after it.
 *
 * The segment layout is four contiguous 0x4000 segments, so s2 is mapbase
 * +0x8000 and the slot is mapbase +0x8048. s2 is mapped PROT_READ, so this
 * load is safe. */
#define ARB_GOT_SLOT_OFF   0x8048   /* segment 2 +0x48                       */
#define ARB_PLT_STUB_OFF   0x356    /* what an unbound slot points back at   */

static int
arbitration_import_is_bound(intptr_t arb_base)
{
    static const struct { unsigned got; unsigned stub; const char *what; } kSlots[] = {
        { 0x8000, 0x0e0, "(unnamed)"                                   },
        { 0x8010, 0x0d0, "(unnamed)"                                   },
        { ARB_GOT_SLOT_OFF, ARB_PLT_STUB_OFF,
          "<-- Initialize's tail call. THIS ONE DECIDES."               },
        { 0x8058, 0x376, "(unnamed)"                                   },
        { 0x8070, 0x3a6, "(unnamed)"                                   },
    };
    int bound = 0;

    LOG("\n--- arbitration: GOT slots, read before anything is called ---\n");
    LOG("    An unbound lazy import still points into its own PLT push/jmp\n"
        "    sequence. Calling one blocks forever - it does not fault and it\n"
        "    does not return. Rule 15. This is why runs 9 and 10 hung.\n\n");

    for (size_t i = 0; i < sizeof kSlots / sizeof *kSlots; i++) {
        uint64_t v        = *(volatile uint64_t *)(arb_base + kSlots[i].got);
        uint64_t stub     = (uint64_t)(arb_base + kSlots[i].stub);
        int      unbound  = (v == stub);

        LOG("  s2+0x%02x = 0x%016llx  %-9s %s\n",
            kSlots[i].got - 0x8000, (unsigned long long)v,
            unbound ? "UNBOUND" : "bound", kSlots[i].what);

        if (kSlots[i].got == ARB_GOT_SLOT_OFF)
            bound = !unbound;
    }

    if (bound)
        LOG("\n  *** THE SLOT IS BOUND. *** One of the modules loaded ahead of\n"
            "  libSceVideoDecoderArbitration this run supplied it. Initialize\n"
            "  is safe to call, and the hang of runs 9 and 10 is explained and\n"
            "  fixed.\n");
    else
        LOG("\n  Still unbound. Initialize WILL hang if called, so it is not\n"
            "  being called - the run continues to the decode instead.\n"
            "  The provider is some module not loaded this run.\n");

    return bound;
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

    /* -- aligned with the only implementation known to actually decode -----
     *
     * Moonlight-ps4 (JaimeJimenezG/Moonlight-ps4, src/orbis/videodec2.h and
     * src/video/decoder_orbis.c) drives this same library successfully, and
     * its configuration is annotated with what was validated on a console.
     * Its struct is byte-identical to the one above - shadPS4 asserts the
     * same 0x48 size - so the two fields below are directly comparable, and
     * both were left at the memset default here, which is NOT what a working
     * client sends:
     *
     *   cpuAffinityMask = 0x3F   all six cores. Zero means "inherit", which
     *                            is legal - the validator accepts it - but a
     *                            decoder whose worker threads inherit a mask
     *                            from a payload process is not the case Sony
     *                            tests. 0x3F is what the working client uses.
     *
     *   optimizeProgressiveVideo = true   the working client sets it; this
     *                            probe has always sent false.
     *
     * Neither is a validated-error field, so neither would ever have shown up
     * as a bad return code - they are exactly the kind of difference that
     * only appears by comparing against something that works. */
    cfg->cpuAffinityMask          = 0x3Full;
    cfg->optimizeProgressiveVideo = 1;
    cfg->checkMemoryType          = 0;
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

/* -- the submitted job, read out of our own memory -------------------------
 *
 * findings.md §7 opened this box statically and stopped here: "the job is a
 * POINTER TO A COMMAND BUFFER; whatever the driver objects to is in that
 * buffer's contents, which the codec layer builds. There is no further handle
 * here ... and the consumer is kernel-side."
 *
 * That is true of the DUMP. It is not true of a LIVE decoder. The VdecCore
 * object lives at decoder+0x78, which is pWorkMemory+0x40000 - memory THIS
 * PROGRAM allocated, registered and can read. So every field the submit reads
 * can simply be looked at after the refusal, including the command buffer the
 * driver rejected. Nothing here calls anything; it is all loads.
 *
 * The layout, from the disassembly of libSceVdecCore +0x2b870:
 *
 *     ioctl(fd, _IOW(0x83, 23, 24), { &job, evq, callerArg, mystery, 0 })
 *
 *     obj+0x0058  ring of 5 command-buffer slots, stride 0x80
 *     obj+0x1898  job.command   - 0, 1 or 2
 *     obj+0x18a0  job.cmdBuffer - a pointer into that ring
 *     obj+0x18a8  job.flag
 *     obj+0x1d50  ring index; the slot used is index % 5
 *     obj+0x1d68  the device file descriptor
 *     obj+0x1d6c  the event-queue id, also passed to the wait after the submit
 *     obj+0x1d70  READ BY THE SUBMIT, WRITTEN NOWHERE IN VDECCORE
 *     obj+0x02c0  the mode; 7 selects command 23, which is what run 8 reported
 *
 * obj+0x1d70 is the interesting one. If it reads back as zero, the object is
 * simply zero-initialised there and the driver expects zero. If it reads back
 * as something structured, some module outside VdecCore fills it in - and
 * that module is not loaded here, which would be a fifth instance of the one
 * failure mode this whole effort keeps rediscovering.
 *
 * SAFETY: the command-buffer pointer is only dereferenced if it lands inside
 * the work-memory block we allocated. A stray pointer is printed, not
 * followed - an execute-only or unmapped read kills the payload outright and
 * a SIGSEGV handler does not rescue it (§9). */
/* Run 12 applied those offsets to decoder+0x78 and every field read back zero
 * - mode 0, fd 0, cmdBuf NULL. That is not a finding about the job; it is the
 * offsets being applied to the WRONG OBJECT. findings.md §7 already says the
 * submit is a virtual call at [[gpdec+0x38]]+0x18, so the object the submit
 * takes is the GpDec device object, reached by a pointer chain, not the
 * VdecCore object the decoder handle points at.
 *
 * Rather than guess the chain, find the object by what is unique about it: it
 * holds the decoder device's FILE DESCRIPTOR at +0x1d68 and the mode at
 * +0x2c0. So enumerate the process's open fds, identify the character devices,
 * and then sweep our own memory for a 32-bit word equal to one of them whose
 * containing structure also has the right mode. Two independent constraints
 * 0x1b08 bytes apart do not coincide by accident.
 *
 * This also answers a question worth having on its own: whether the decoder
 * device is open in this process at all. */

/* -- video out, opened before anything decodes -----------------------------
 *
 * The one ordering step the working client does that this probe never has.
 *
 * Moonlight-ps4 (src/video/decoder_orbis.c, dr_setup) calls
 * video_present_init() - which is sceVideoOutOpen(userId, BUS_MAIN, 0, NULL)
 * plus sceVideoOutSetFlipRate - BEFORE it allocates the compute queue and
 * BEFORE CreateDecoder. Its comment gives the reason and it is not cosmetic:
 *
 *     "Present BEFORE CreateDecoder (validated stability) and BEFORE creating
 *      the compute queue: if YCbCr hard-fails we left the queue alive and
 *      suspending the app triggered CPU_FAULT_SUBMITDONE_TIMEOUT."
 *
 * CPU_FAULT_SUBMITDONE_TIMEOUT is a SUBMIT fault. That is the same stage of
 * the pipeline our decode dies at - the driver refusing a submitted job with
 * errno 5200 - and it says the display pipeline and the decode submit path
 * are coupled.
 *
 * Which is plausible on its own terms: the decoder writes into GPU memory and
 * moves frames with compute shaders (SceVdecShaderFrameCopyY and friends,
 * findings.md section 8). Opening video out is how a process acquires its
 * GPU/display context on this platform. A process that never opened one has
 * nothing for the decoder's GPU work to belong to - and a driver would be
 * entitled to refuse a job from it.
 *
 * Every probe from Phase 4 onward has skipped this. So has every decode
 * attempt. It is the largest remaining structural difference between this
 * program and one that is known to decode.
 *
 * The user id matters too. Moonlight passes a real logged-in user and only
 * falls back to a constant. This probe measured on 2026-08-11 that the app
 * slot HAS a logged-in user (id 513995993 from GetLoginUserIdList), so it
 * passes that, and falls back to 0xFF - the system user - which is what
 * videoout_test uses successfully.
 *
 * Failure here is reported and does not stop the run: if video out cannot be
 * opened, that is itself worth knowing, and the decode below still tells us
 * whether anything changed. */
/* -- REMOVED: sceVideoOutOpen -----------------------------------------------
 *
 * THIS KERNEL-PANICKED THE CONSOLE on 2026-08-11. Do not put it back.
 *
 * The idea came from Moonlight-ps4, which opens video out before it creates a
 * decoder and ties that ordering to a SUBMIT fault - the same pipeline stage
 * our decode dies at. The reasoning was sound; the transfer was not. Moonlight
 * is a TITLE that owns its process. This payload is injected into a BORROWED
 * hbldr app slot that already owns the display pipeline.
 *
 * The log that survived on /mnt/usb0 says exactly what happened:
 *
 *     sceVideoOutOpen(user 513995993, BUS_MAIN) -> 0x80290001 FAILED
 *     sceVideoOutOpen(user 0xFF,      BUS_MAIN) -> 1309671680 "ok"
 *     sceVideoOutSetFlipRate(60Hz) -> 0x00000000
 *     --- compute queue ---
 *     QueryComputeMemoryInfo -> 0  size 4805120
 *     queue memory 4805120 B WB_ONION -> 0 virt 2000a0000
 *     <panic - nothing further>
 *
 * The next call was sceVideodec2AllocateComputeQueue, which bottoms out in
 * sceGnmMapComputeQueue. THAT CALL HAD SUCCEEDED IN EVERY PREVIOUS RUN. The
 * only new thing in the process was an open video-out handle. Opening video
 * out and then mapping a GPU compute queue is what wedged the GPU driver.
 *
 * Two things worth keeping from it:
 *
 *   - The real logged-in user is REFUSED with 0x80290001. The app slot already
 *     holds the display pipeline; a second client is not welcome.
 *   - The 0xFF system-user retry returned 1309671680 = 0x4E100000, which is
 *     NOT a handle - real ones are small integers. The "h < 0" check treated
 *     garbage as success. Validate handles by RANGE, not by sign.
 */

/* -- REMOVED: sceVideoOutOpen ---------------------------------------------- */

/* Every open fd, and what kind of file it is. */
static void
dump_open_fds(void)
{
    int shown = 0;

    LOG("\n--- open file descriptors in this process ---\n");
    LOG("    The decoder device is opened by libSceVdecCore at +0x2c470, which\n"
        "    also sets the event-queue id. If no character device is open here,\n"
        "    the submit had no device to talk to and errno 5200 is explained.\n\n");

    for (int fd = 0; fd < 256; fd++) {
        struct stat st;

        if (fcntl(fd, F_GETFD) == -1)
            continue;

        memset(&st, 0, sizeof st);
        if (fstat(fd, &st) != 0) {
            LOG("  fd %3d  open, fstat failed\n", fd);
            shown++;
            continue;
        }

        LOG("  fd %3d  mode 0%06o  rdev 0x%llx  size %lld  %s\n", fd,
            (unsigned)st.st_mode, (unsigned long long)st.st_rdev,
            (long long)st.st_size,
            S_ISCHR(st.st_mode)  ? "CHARACTER DEVICE" :
            S_ISSOCK(st.st_mode) ? "socket"           :
            S_ISFIFO(st.st_mode) ? "pipe"             :
            S_ISDIR(st.st_mode)  ? "directory"        :
            S_ISREG(st.st_mode)  ? "regular file"     : "other");
        shown++;
    }

    if (!shown)
        LOG("  none - which cannot be right, so treat this probe as broken.\n");
}

/* The two entries of the +0x2b870 jump table this probe can encounter, as
 * (index at +0x1d08, mode at +0x2c0) pairs. Index 5 -> mode 7 -> ioctl 23 is
 * the route every run has taken; index 1 -> mode 0x10 -> ioctl 24 is the one
 * the VdecCore writers at +0x13e58 / +0x158d8 would select. */
static const uint32_t kExpectIdx[2] = { 5u, 1u };
static const uint32_t kModeFor[2]   = { 7u, 0x10u };

/* Is this fd open in this process, and is it a character device? The decoder
 * device is a chardev, so a candidate whose fd is neither cannot be it. */
static int
fd_is_chardev(int fd)
{
    struct stat st;

    if (fd < 0 || fstat(fd, &st) != 0)
        return 0;

    return S_ISCHR(st.st_mode);
}

/* Sweep a block for the GpDec device object, looking for one specific mode. */
static const unsigned char *
find_device_object(const dmem_block *blk, const char *what, uint32_t wantMode)
{
    const unsigned char *base = (const unsigned char *)blk->vaddr;
    const unsigned char *found = NULL;
    size_t len = blk->len;
    int    hits = 0;

    if (!base || len < 0x2000)
        return NULL;

    /* The object must have at least 0x2000 bytes ahead of it for +0x1d70 and
     * the fields around it to be inside the block. */
    for (size_t off = 0; off + 0x2000 <= len; off += 8) {
        const unsigned char *cand = base + off;
        uint32_t mode = *(const volatile uint32_t *)(cand + 0x2c0);
        int32_t  fd   = *(const volatile int32_t  *)(cand + 0x1d68);
        uint32_t idx  = *(const volatile uint32_t *)(cand + 0x1d08);

        if (mode != wantMode || fd <= 2 || fd > 4096)
            continue;

        /* -- CORROBORATION, and the reason it exists --------------------------
         *
         * The first cut of this search accepted mode 7 OR mode 0x10, on the
         * reasoning that insisting on 7 could only ever confirm the answer we
         * already had. That reasoning was right and the change was still wrong:
         * mode 0x10 is the value SIXTEEN, and the work arena is full of it -
         * the run of 2026-08-13 matched five "objects" inside a region that is
         * a solid repeating array of 10 00 00 00, and reported the submit path
         * had moved. It had not. Every field read out of that address was 16
         * because every word there is 16.
         *
         * The predicate's power was never "mode == 7". It was that 7 is RARE.
         * Widening it to a common value threw the power away, so the value has
         * to be paid for some other way. Two checks do it, and both are free:
         *
         *   1. The jump table has to agree with itself. [0x1d08] indexes it and
         *      [0x2c0] is its output, so index 5 must accompany mode 7 and
         *      index 1 must accompany mode 0x10. The false match read index 0
         *      with mode 16, which the table forbids.
         *   2. The fd has to be one this process actually has open, and it has
         *      to be a character device. The false match claimed fd 119; the fd
         *      dump printed two lines below it showed the process owned nothing
         *      above 17. */
        if (!(idx == kExpectIdx[0] && mode == kModeFor[0]) &&
            !(idx == kExpectIdx[1] && mode == kModeFor[1]))
            continue;

        if (!fd_is_chardev(fd))
            continue;

        if (++hits <= 4) {
            LOG("  candidate in %s at +0x%zx  (addr %p): index %u, mode %u,"
                " fd %d (open chardev)\n",
                what, off, (const void *)cand, idx, mode, fd);
            if (!found)
                found = cand;
        }
    }

    if (hits > 4)
        LOG("  ...and %d more candidates in %s\n", hits - 4, what);
    if (!hits)
        LOG("  no candidate in %s\n", what);

    return found;
}

/* -- Why sceVideodec2Reset fails, answered by pointer-walking not by luck ----
 *
 * Reset returns 0x811D0111 on every call and never clears the error latch, and
 * reading it offline (findings §7) says the refusal is not its own:
 *
 *     sceVideodec2Reset  +0x30f0   passes magic, lock, argument gates
 *       -> sceVdecCoreResetDecoder +0x1a40   arg2 = 0, explicitly permitted
 *          -> rdi = [vdeccore+0x140]         the GpDec object
 *          -> GpDec reset 0x800978550        FAILS on its first check
 *
 * That function has exactly three ways to fail, and each one is a different
 * statement about the object at [vdeccore+0x140]:
 *
 *     line 0x1645   rdi == NULL                    - no GpDec object at all
 *     line 0x164C   memcmp(rdi, magic, 0x10) != 0  - not a valid GpDec object
 *     line 0x1653   the lock at rdi+0x20 failed    - valid, but held
 *
 * (The check is memcmp: the helper is called 42 times, and two adjacent sites
 * compare one 16-byte stack buffer against two different .rodata constants and
 * branch on equality. Nothing else fits - you cannot lock or copy into rodata.)
 *
 * The console prints WHICH line fired, but only on stdout via the hbldr pipe,
 * which is easy to lose and was lost once already. This does not need the
 * console's help: the VdecCore object is in memory we allocated, so the pointer
 * at +0x140 can simply be read, and the three cases told apart here. */
static void
diagnose_gpdec_object(void *decoder, const unsigned char *located,
                      const dmem_block *work, const dmem_block *pool)
{
    const unsigned char *vdec, *gpdec;
    uintptr_t lo, hi, plo, phi;

    if (!decoder || !work || !work->vaddr)
        return;

    LOG("\n--- why Reset fails: the GpDec object at [vdeccore+0x140] ---\n");

    vdec = *(const unsigned char *const *)((const unsigned char *)decoder + 0x78);
    lo  = (uintptr_t)work->vaddr;  hi  = lo + work->len;
    plo = pool ? (uintptr_t)pool->vaddr : 0;
    phi = pool ? plo + pool->len : 0;

    if (!vdec) {
        LOG("    decoder+0x78 is NULL - there is no VdecCore object to walk.\n");
        return;
    }
    if (!(((uintptr_t)vdec >= lo && (uintptr_t)vdec + 0x148 <= hi) ||
          (plo && (uintptr_t)vdec >= plo && (uintptr_t)vdec + 0x148 <= phi))) {
        LOG("    VdecCore object %p is outside memory we allocated, so +0x140\n"
            "    is NOT dereferenced - a bad read here kills the payload.\n",
            (const void *)vdec);
        return;
    }

    gpdec = *(const unsigned char *const *)(vdec + 0x140);
    LOG("    [vdeccore+0x140] = %p\n", (const void *)gpdec);

    if (!gpdec) {
        LOG("\n    *** IT IS NULL. That is GpDec line 0x1645, and it means the\n"
            "    reset never had an object to work on. Reset's failure is then\n"
            "    a lifetime problem - the GpDec object was never created, or was\n"
            "    torn down by the refused decode - and NOT a permissions or an\n"
            "    entitlement problem. ***\n");
        return;
    }

    /* These are two DIFFERENT objects and that is correct, not a discrepancy.
     * [vdeccore+0x140] is the GpDec decoder object - magic at +0, state at
     * +0x40, locks at +0x10/+0x20/+0x30. The submit runs on the GpDec DEVICE
     * object, reached from it by the virtual call at [[gpdec+0x38]]+0x18, and
     * that is the one carrying the fd at +0x1d68 and the mode at +0x2c0. An
     * earlier version of this line called the difference an error; it is the
     * pointer chain working as documented. */
    if (located)
        LOG("    the submit's device object:         %p  (%s)\n",
            (const void *)located,
            gpdec == located
                ? "the same object - unexpected, the chain should separate them"
                : "a different object, as the [[gpdec+0x38]]+0x18 chain implies");

    if (!(((uintptr_t)gpdec >= lo && (uintptr_t)gpdec + 0x48 <= hi) ||
          (plo && (uintptr_t)gpdec >= plo && (uintptr_t)gpdec + 0x48 <= phi))) {
        LOG("    ...but it is outside memory we allocated, so its head is NOT\n"
            "    read. Not being able to read it is not evidence against it:\n"
            "    VdecCore is entitled to keep it on its own heap.\n");
        return;
    }

    LOG("\n    the 16 bytes GpDec memcmps against its magic (line 0x164C):\n");
    hexdump(gpdec, 0x10, "        ");
    LOG("\n    gpdec+0x40 state = %u  (line 0xC22 refuses states 0, 4 and 5)\n",
        *(const volatile uint32_t *)(gpdec + 0x40));
    LOG("    gpdec+0x20 lock  = 0x%016llx  (line 0x1653 is this lock failing)\n",
        (unsigned long long)*(const volatile uint64_t *)(gpdec + 0x20));
    LOG("\n    If those 16 bytes look like a signature - printable, or stable\n"
        "    across runs - the magic check passes and the refusal is the lock\n"
        "    or the state. If they look like zeros or heap debris, line 0x164C\n"
        "    is the answer and the pointer at +0x140 is stale.\n");
}

/* Find the GpDec device object once, so every AU afterwards can be read
 * against the SAME object rather than re-searched with a predicate that
 * assumes the answer. Returns NULL if it is not in memory we allocated. */
static const unsigned char *
locate_device_object(void *decoder, const dmem_block *work,
                     const dmem_block *pool)
{
    const unsigned char *obj;

    if (!decoder || !work || !work->vaddr)
        return NULL;

    LOG("\n--- locating the GpDec device object the submit operates on ---\n");
    LOG("    decoder %p  ->  VdecCore object %p\n", decoder,
        *(const void *const *)((const unsigned char *)decoder + 0x78));
    LOG("    A candidate must satisfy all three: the mode at +0x2c0, an index\n"
        "    at +0x1d08 that the jump table maps TO that mode, and an fd at\n"
        "    +0x1d68 this process really has open as a character device.\n");

    /* Mode 7 first, because it is rare and it is the answer every previous run
     * got. Only if it is absent is mode 0x10 worth looking for - searching for
     * the common value first is what produced the false positive. */
    LOG("\n  pass 1: index 5 / mode 7 -> ioctl command 23 (the known route)\n");
    obj = find_device_object(work, "work memory", 7u);
    if (!obj && pool)
        obj = find_device_object(pool, "frame pool", 7u);

    if (!obj) {
        LOG("\n  pass 2: index 1 / mode 0x10 -> ioctl command 24 (the route\n"
            "          Phase 6b.1 is looking for)\n");
        obj = find_device_object(work, "work memory", 0x10u);
        if (!obj && pool)
            obj = find_device_object(pool, "frame pool", 0x10u);
        if (obj)
            LOG("\n  *** FOUND ON MODE 0x10, AND IT PASSED BOTH CORROBORATION\n"
                "      CHECKS. That is the Phase 6b.1 result, not an artefact. ***\n");
    }

    if (!obj)
        LOG("\n  The object is not in memory we allocated, so VdecCore keeps it\n"
            "  on its own heap. The fd list above still says whether the device\n"
            "  is open, which is the half of the question that matters most.\n");

    return obj;
}

/* -- PHASE 6b.1: has the submit path moved? --------------------------------
 *
 * The one question this run exists to answer. libSceVdecCore +0x2b870 picks
 * the ioctl command from a six-entry jump table indexed by [obj+0x1d08]:
 * index 5 -> mode 7 -> command 23, which is what every run so far has taken;
 * index 1 -> mode 0x10 -> command 24, which nothing has ever taken.
 *
 * Run 14 swept resource class and pipeline depth and concluded the path was
 * fixed. It is not: two writers INSIDE VdecCore, at +0x13e58 and +0x158d8,
 * store the literal 1 into that field, guarded by an equality between two
 * counters in the codec context and followed by a 0x204-byte memcpy into
 * [obj+0x1448]. Neither is reachable by configuration - what would reach them
 * is parsed bitstream state, which is why this feeds every access unit in the
 * stream instead of stopping at the first refusal.
 *
 * So the selector is read after EVERY AU, and the 0x1448 window with it: if
 * the writers ever fire, both change together and one confirms the other. */
typedef struct {
    uint32_t      lastIdx;              /* 5 on every run before this one    */
    int           primed;               /* has a baseline been taken yet?    */
    int           moved;                /* did it EVER leave 5?              */
    unsigned char last1448[0x20];
} selector_watch;

static void
log_submit_selector(const unsigned char *obj, const char *when,
                    selector_watch *w)
{
    uint32_t idx, mode;
    unsigned cmd;
    int      changed1448;

    if (!obj)
        return;

    idx  = *(const volatile uint32_t *)(obj + 0x1d08);
    mode = *(const volatile uint32_t *)(obj + 0x02c0);
    cmd  = mode == 7 ? 23u : mode == 0x10 ? 24u : 22u;

    changed1448 = memcmp(w->last1448, obj + 0x1448, sizeof w->last1448) != 0;
    memcpy(w->last1448, obj + 0x1448, sizeof w->last1448);

    LOG("    [6b.1] %-16s obj+0x1d08 = %u -> mode %u -> ioctl command %u%s\n",
        when, idx, mode, cmd,
        idx == 1 ? "   *** THE OTHER PATH ***" : "");

    if (w->primed && idx != w->lastIdx) {
        LOG("    [6b.1] *** THE SELECTOR MOVED: %u -> %u ***\n"
            "           Run 14's \"the submit path does not vary\" is dead, and\n"
            "           the writers at VdecCore +0x13e58 / +0x158d8 are on the\n"
            "           same object the submit uses. Findings section 7 [H] is\n"
            "           settled in the affirmative.\n", w->lastIdx, idx);
    }
    if (idx != 5)
        w->moved = 1;

    w->lastIdx = idx;

    /* The first read is a baseline, not a change - saying otherwise would
     * manufacture a signal out of the zeroed initial state. */
    if (changed1448) {
        LOG("    [6b.1] obj+0x1448 %s - the 0x204-byte memcpy that\n"
            "           accompanies a write of 1 to the selector lands here:\n",
            w->primed ? "CHANGED" : "baseline");
        hexdump(obj + 0x1448, sizeof w->last1448, "           ");
    }

    w->primed = 1;
}

static void
dump_submitted_job(void *decoder, const unsigned char *obj,
                   const dmem_block *work, const dmem_block *pool)
{
    uint32_t  mode, ringIdx, jobCmd, jobFlag, evq, mystery;
    int32_t   fd;
    uintptr_t cmdBuf;

    if (!obj)
        return;

    (void)decoder;
    dump_open_fds();

    mode    = *(const volatile uint32_t *)(obj + 0x02c0);
    ringIdx = *(const volatile uint32_t *)(obj + 0x1d50);
    jobCmd  = *(const volatile uint32_t *)(obj + 0x1898);
    cmdBuf  = *(const volatile uintptr_t *)(obj + 0x18a0);
    jobFlag = *(const volatile uint32_t *)(obj + 0x18a8);
    fd      = *(const volatile int32_t  *)(obj + 0x1d68);
    evq     = *(const volatile uint32_t *)(obj + 0x1d6c);
    mystery = *(const volatile uint32_t *)(obj + 0x1d70);

    /* -- WHICH submit path this is, and who chose it ----------------------
     *
     * libSceVdecCore +0x2b870 picks the ioctl command from the mode at
     * +0x2c0: mode 7 -> command 23, mode 0x10 -> command 24, anything else
     * -> command 22. And the mode is not configured directly. It comes out
     * of a six-entry jump table indexed by [obj+0x1d08]:
     *
     *     [0x1d08]  0 -> mode 0     3 -> mode 3 (or 0x80000003)
     *               1 -> mode 0x10  4 -> mode 1
     *               2 -> mode 4     5 -> mode 7      <- ours
     *
     * [0x1d08] is written by a four-field setter at +0x288f0 that VdecCore
     * exports and that the CODEC MODULE calls - so the codec layer chooses
     * the submit path, and the value it chose is the most useful single
     * number in this dump. (This comment said +0x8f0. That was a
     * transcription slip of 0x28000, corrected 2026-08-13 against the indexed
     * listing, where every other offset in this section resolves exactly.)
     *
     * The other two sites that write it are at +0x13e58 and +0x158d8, both
     * storing the literal 1 - ioctl command 24 - and neither is selected by
     * configuration. Phase 6b.1 above is the experiment that tests whether
     * bitstream state reaches them. */
    LOG("\n--- the job that was submitted ---\n");
    LOG("    obj+0x1d08 mode index  = %u  -> mode %u -> ioctl command %d\n",
        *(const volatile uint32_t *)(obj + 0x1d08), mode,
        mode == 7 ? 23 : mode == 0x10 ? 24 : 22);
    LOG("    obj+0x1d18/0x1d1c/0x1d20 = 0x%08x 0x%08x 0x%08x  (set together\n"
        "                               with the mode index by the codec)\n",
        *(const volatile uint32_t *)(obj + 0x1d18),
        *(const volatile uint32_t *)(obj + 0x1d1c),
        *(const volatile uint32_t *)(obj + 0x1d20));
    LOG("    obj+0x1d4c/0x1d54          = 0x%08x 0x%08x\n",
        *(const volatile uint32_t *)(obj + 0x1d4c),
        *(const volatile uint32_t *)(obj + 0x1d54));
    LOG("    obj+0x02c0 mode        = %u\n", mode);
    LOG("    obj+0x1d68 fd          = %d\n", fd);
    LOG("    obj+0x1d6c event queue = 0x%08x\n", evq);
    LOG("    obj+0x1d70 mystery     = 0x%08x   %s\n", mystery,
        mystery ? "*** NON-ZERO - something DID fill it in ***"
                : "zero - either expected, or nobody filled it in");
    LOG("    obj+0x1d50 ring index  = %u  -> slot %u of 5\n",
        ringIdx, ringIdx % 5u);
    LOG("    obj+0x1898 job.command = %u\n", jobCmd);
    LOG("    obj+0x18a0 job.cmdBuf  = 0x%llx\n", (unsigned long long)cmdBuf);
    LOG("    obj+0x18a8 job.flag    = %u\n", jobFlag);

    LOG("\n    the command ring at obj+0x58, 5 slots of 0x80:\n");
    for (unsigned s = 0; s < 5; s++) {
        const unsigned char *slot = obj + 0x58 + s * 0x80;
        int nonzero = 0;

        for (unsigned b = 0; b < 0x80; b++)
            if (slot[b]) { nonzero = 1; break; }

        LOG("      slot %u%s%s\n", s,
            nonzero ? ":" : " is entirely ZERO",
            (s == ringIdx % 5u) ? "   <-- the slot this submit used" : "");
        if (nonzero)
            hexdump(slot, 0x80, "        ");
    }

    /* Only follow the command-buffer pointer into memory we own. An unmapped
     * or execute-only read kills the payload outright (§9). */
    {
        uintptr_t wlo = (uintptr_t)work->vaddr, whi = wlo + work->len;
        uintptr_t plo = pool ? (uintptr_t)pool->vaddr : 0;
        uintptr_t phi = pool ? plo + pool->len : 0;
        int safe = (cmdBuf >= wlo && cmdBuf + 0x100 <= whi) ||
                   (plo && cmdBuf >= plo && cmdBuf + 0x100 <= phi);

        if (safe) {
            LOG("\n    the command buffer the driver rejected (0x100 bytes):\n");
            hexdump((const void *)cmdBuf, 0x100, "        ");
        } else if (cmdBuf) {
            LOG("\n    job.cmdBuf 0x%llx is outside memory we allocated, so it\n"
                "    is NOT dereferenced - a bad read here kills the payload.\n",
                (unsigned long long)cmdBuf);
        } else {
            LOG("\n    *** job.cmdBuf IS NULL *** - the submit handed the driver\n"
                "    a job with no command buffer.\n");
        }
    }

    LOG("\n    a window around the object head, for orientation:\n");
    hexdump(obj + 0x280, 0x80, "        ");
    LOG("\n    and around the submit fields at +0x1d40:\n");
    hexdump(obj + 0x1d40, 0x60, "        ");

    /* -- REMOVED: the ioctl errno-space probe -----------------------------
     *
     * This block issued deliberately malformed ioctls on the decoder fd -
     * undefined command numbers, a NULL arg, a zeroed arg - to learn whether
     * errno 5200 is a verdict on the JOB or a blanket refusal of the PROCESS.
     *
     * It never ran. The console kernel-panicked earlier in the same build, at
     * sceVideodec2AllocateComputeQueue, so this is not what panicked it, and
     * the question it asked is still open and still worth answering.
     *
     * Removed regardless. Fuzzing a proprietary kernel GPU driver was called
     * "low risk by construction" here, on the reasoning that drivers validate
     * their inputs. That reasoning had nothing behind it. A panic costs about
     * fifty minutes of someone else's time. If this experiment is worth
     * running later, it gets proposed and agreed BEFORE it is built.
     */
}

/* Queries, allocates, creates, decodes, flushes, deletes, releases. Owns every
 * buffer it uses, so an attempt that fails leaves nothing behind for the next.
 * Returns the number of valid pictures the decoder reported. */
static int
attempt_decode(query_decoder_meminfo_fn query, create_decoder_fn create,
               delete_decoder_fn del, decode_fn decode, flush_fn flush,
               get_picture_info_fn getPic, map_direct_memory_fn mapDirect,
               reset_fn reset, const char *label, framing_t framing,
               uint32_t resourceType, uint32_t depth, unsigned auCount,
               void *queue, int controls, int dumpFrame)
{
    SceVideodec2DecoderConfigInfo cfg;
    SceVideodec2DecoderMemoryInfo mem;
    dmem_block  blocks[3 + MAP_BLOCKS] = {{0}};  /* work, pool, input, targets */
    dmem_block *work = &blocks[0], *pool = &blocks[1], *inbuf = &blocks[2];
    dmem_block *targets = &blocks[3];
    int         mapped = 0;
    int         dumpedJob = 0;
    const unsigned char *obj = NULL;             /* the GpDec device object   */
    selector_watch       watch = { 5, 0, 0, {0} };
    unsigned             fed = 0;                /* AUs actually handed over  */
    int                  diagnosedReset = 0;
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

    /* -- geometry and depth, from the working client -----------------------
     *
     * Two more differences from Moonlight-ps4, both annotated there as
     * console-validated:
     *
     *   maxFrameHeight must be MACROBLOCK-ALIGNED. Its comment is explicit -
     *   "1088: macroblock-align; 1080 in config -> slow Decode / rare paths".
     *   This probe has always configured 1080. The command buffer dumped in
     *   run 13 shows the module padding it to 1088 (0x440) internally anyway,
     *   so the two disagree about the height right up to the submit.
     *
     *   maxDpbFrameCount 4 rather than 16, and decodePipelineDepth 2 rather
     *   than 1 - "depth=1 serialises submit->wait against the compute queue
     *   every frame". Depth 1 was chosen here as "the shortest path from one
     *   access unit to one picture", which is reasonable and is not what a
     *   working client does. */
    cfg_init(&cfg, resourceType, CODEC_AVC, TEST_STREAM_PROFILE,
             TEST_STREAM_LEVEL, TEST_STREAM_WIDTH,
             (TEST_STREAM_HEIGHT + 15u) & ~15u, 4, depth, queue);

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
            fed++;

            {
                char what[48];
                snprintf(what, sizeof what, "Decode(AU %u)", i);
                report_output(&ctx, what, rc, &out, &fb);
            }

            /* Locate the object once, on the first AU, whatever the result -
             * every later read is then against the same address rather than a
             * fresh search whose predicate presumes the answer. */
            if (!obj)
                obj = locate_device_object(dec, work, pool);

            /* The refusal is the most informative moment there is: the object
             * still holds exactly what was handed to the driver. Dumped once,
             * on the first failure only, so a repeated error does not bury the
             * log. */
            if (rc && !dumpedJob) {
                dumpedJob = 1;
                dump_submitted_job(dec, obj, work, pool);
            }

            {
                char when[24];
                snprintf(when, sizeof when, "after AU %u", i);
                log_submit_selector(obj, when, &watch);
            }

            /* -- PHASE 6b.1: do NOT stop at the first refusal ---------------
             *
             * Every run before this one broke here, so AU 0 - the IDR - is the
             * only access unit the hardware has ever been shown. That is
             * exactly the wrong stream for the question now being asked: the
             * two writers that would move the submit onto ioctl command 24 are
             * guarded by an equality between two counters in the codec context,
             * and one access unit cannot make two counters disagree.
             *
             * A failed Decode latches decoder+0x48/0x4c, after which every
             * further Decode is refused in software with 0x811D0300/0304 and
             * never reaches VdecCore at all - so continuing is only meaningful
             * with a Reset in between. Reset was resolved in every run so far
             * and deliberately never called; this is what it is for.
             *
             * The cost of being wrong is one more error line in the log. */
            if (rc) {
                int rrc = reset ? reset(dec) : -1;

                LOG("    [6b.1] AU %u refused; sceVideodec2Reset -> 0x%08x %s\n",
                    i, rrc, reset ? vdec2_err((uint32_t)rrc) : "(not resolved)");

                /* Once, on the first failure: walk to the object GpDec is
                 * actually refusing. Reading it costs nothing and tells the
                 * three failure lines apart without the stdout diagnostics. */
                if (rrc && !diagnosedReset) {
                    diagnosedReset = 1;
                    diagnose_gpdec_object(dec, obj, work, pool);
                }

                if (rrc)
                    LOG("    [6b.1] Reset did NOT clear the latch. The next AU is\n"
                        "           expected to be refused in software with\n"
                        "           0x811D0300/0304 without reaching VdecCore -\n"
                        "           and if it is refused with something else, that\n"
                        "           is worth more than this experiment was.\n"
                        "           Feeding it anyway: the cost is one log line.\n");

                log_submit_selector(obj, "after Reset", &watch);
            }
        }

        /* Report what was actually fed, not what was asked for. The first cut
         * of this line printed auCount and announced "fed 8 of 8" after the
         * decoder had refused AU 0 and taken no further input at all. */
        LOG("\n  [6b.1] fed %u of the %u access units asked for; the submit"
            " selector %s.\n", fed, auCount,
            !obj          ? "could not be read - the object was never located"
            : watch.moved ? "MOVED off 5 - see the marked lines above"
                          : "never moved off 5 (ioctl command 23)");
        if (fed < 2)
            LOG("  [6b.1] Fewer than two AUs reached the decoder, so the guard\n"
                "         this experiment targets - an equality between two\n"
                "         counters in the codec context - cannot have been\n"
                "         disturbed. This run does NOT test 6b.1's question.\n");

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
    /* Phase 6b.1 needs the WHOLE stream, not a sample of it: the counters that
     * would move the submit selector cannot disagree while only one access
     * unit has ever been parsed. "one" still narrows it back down by hand. */
    unsigned auCount   = TEST_STREAM_AU_COUNT;
    int      dumpFrame = 0;
    int      tryAvcc   = 1;
    int      useArb    = 1;
    int      arbUp     = 0;
    int      arbBound  = 0;          /* is Initialize's import resolved?     */
    int32_t  loginUser = -1;         /* real user id, if the slot has one     */
    uint32_t dynh_arb  = 0;
    intptr_t arb_base  = 0;
    int      have_control = 0, queue_ok = 0;
    int      annexbPics = 0, avccPics = 0;
    int      arbPics    = 0;         /* pictures decoded WITH arbitration up */
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

    /* -- the user session, re-measured IN THE APP SLOT -------------------- *
     * findings.md section 2 records "a payload has no user session", from
     * sceUserServiceGetInitialUser returning 0x80940004. That measurement came
     * from decoder_test, which is deployed with deploy.sh - i.e. it ran in
     * SceSpZeroConf, NOT the app slot. It is the same error the 41 MiB memory
     * "ceiling" was: a property of the wrong host process, recorded as a
     * property of payloads. Standing rule 12 exists because of that one, and
     * it was not applied here.
     *
     * EVO Player, in this slot, calls sceUserServiceInitialize and then opens
     * the pad with the id it gets back - so a session plainly IS available
     * here. None of the Phase 4-6 probes ever called Initialize.
     *
     * That matters because arbitration blocks on what looks like an IPC, and
     * "no session" was the hypothesis for why. If the session simply was never
     * established in-process, the block may not be structural at all. */
    LOG("\n--- user session, in the app slot ---\n");
    {
        int32_t user_id = -1;
        int     users[4] = {0};
        int     uinit, ulist, uget;

        uinit = sceUserServiceInitialize(NULL);
        LOG("  sceUserServiceInitialize      -> 0x%08x\n", uinit);

        ulist = sceUserServiceGetLoginUserIdList(users);
        LOG("  sceUserServiceGetLoginUserIdList -> 0x%08x  ids %d %d %d %d\n",
            ulist, users[0], users[1], users[2], users[3]);

        /* Recorded because it corrects findings.md section 2. It is NOT
         * used to open video out - see the REMOVED block above. */
        if (ulist == 0 && users[0] > 0)
            loginUser = users[0];

        uget = sceUserServiceGetInitialUser(&user_id);
        LOG("  sceUserServiceGetInitialUser  -> 0x%08x  user_id %d\n",
            uget, user_id);

        if (uget == 0 || users[0] > 0)
            LOG("\n  *** THERE IS A USER SESSION HERE ***\n"
                "  findings.md section 2 says otherwise, and it was measured in\n"
                "  SceSpZeroConf under deploy.sh. Correct that entry.\n");
        else
            LOG("\n  no user session even after Initialize, in the app slot.\n"
                "  The findings entry stands, and the arbitration hypothesis\n"
                "  with it.\n");
    }

    /* -- arbitration: the GOT slot only. NOTHING IS CALLED HERE. ----------- *
     * Runs 9 and 10 both called Initialize at this point in the program and
     * both hung, taking the entire decode with them - the log stops at the
     * last control and the four gates cleared in runs 1-8 were never
     * re-exercised. That is standing rule 9 violated: the riskiest call in the
     * probe ran before everything cheap.
     *
     * So the order is inverted. Here we only READ the GOT slot, which is free
     * and cannot block. The decode - which is known to return an error rather
     * than hang - runs next. Arbitration is attempted at the very END, and
     * only if the slot is bound. */
    if (kernel_dynlib_handle(getpid(), "libSceVideoDecoderArbitration.sprx",
                             &dynh_arb) == 0) {
        arb_base = kernel_dynlib_mapbase_addr(getpid(), dynh_arb);
        LOG("\nlibSceVideoDecoderArbitration base 0x%lx\n",
            (unsigned long)arb_base);
        arbBound = arbitration_import_is_bound(arb_base);

        /* Run 11 proved this returns rather than blocks once the import is
         * bound: Initialize, Enable and both AcceptEvent calls all came back.
         * So it now runs BEFORE the decode, which is the order AvPlayer uses
         * and which makes every attempt below an arbitrated one. It is still
         * gated on the slot - an unbound import is never called. */
        if (useArb && arbBound)
            arbUp = arbitration_bring_up(dynh_arb, arb_base);
    } else {
        LOG("\nno dynlib handle for libSceVideoDecoderArbitration.sprx\n");
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
    (void)a_getavc; (void)a_gethevc; (void)a_getvp9;

    /* Resolved and deliberately NOT called: MapMemory's one import is unbound
     * in the dump, which is the shape of a silent indefinite block. Logging its
     * offset records that it was considered and rejected. */
    (void)resolve(dynh, base, "sceVideodec2MapMemory", 0xe0);
    LOG("    ^ resolved but never called - its GOT slot (s2 +0x60) is unbound.\n"
        "      MapDirectMemory (+0x340) is the bound route to the same layer.\n");

    if (!a_query || !a_create || !a_delete || !a_decode || !a_flush ||
        !a_getpic || !a_mapdirect || !a_query_cm || !a_alloc_cq || !a_rel_cq ||
        !a_reset) {
        LOG("\nFATAL: an entry point did not resolve\n");
        return EXIT_FAILURE;
    }

    query_decoder_meminfo_fn query    = (query_decoder_meminfo_fn)a_query;
    create_decoder_fn        create   = (create_decoder_fn)a_create;
    delete_decoder_fn        del      = (delete_decoder_fn)a_delete;
    decode_fn                decode   = (decode_fn)a_decode;
    flush_fn                 flush    = (flush_fn)a_flush;
    /* Resolved since Phase 5 and never called until Phase 6b.1, which needs it
     * to clear the error latch between access units. */
    reset_fn                 reset    = (reset_fn)a_reset;
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
    /* -- the configuration sweep ------------------------------------------
     *
     * Run 11 settled the framing question that this phase was built around:
     * Annex-B reaches the hardware submit and is refused there, while AVCC is
     * rejected EARLIER, in software, with 0x811D0303. A bitstream the decoder
     * will not even look at cannot be the one it wants, so Annex-B is the
     * right framing and AVCC is dropped.
     *
     * What is swept instead is the thing the job dump says actually selects
     * the route into the driver: the resource class and the pipeline depth.
     * Each attempt owns and frees its own ~90 MiB, and each prints the mode
     * index the codec layer chose. If any of them lands on a mode index other
     * than 5, that is a different ioctl command and a genuinely different
     * experiment - and it costs nothing to find out here rather than in
     * another deploy. */
    {
        static const struct {
            const char *label;
            uint32_t    res;
            uint32_t    depth;
        } kSweep[] = {
            /* Run 14 swept all four of these and every one produced mode
             * index 5, mode 7, command 23 and errno 5200 - identical down to
             * the field. The configuration does not select the route, so the
             * sweep is collapsed back to one and the deploy is spent on the
             * errno question instead. */
            { "class 0xb6c8, depth 2 (Moonlight's)", RES_STD, 2 },
            { "class 0xb6c8, depth 1",              RES_STD, 1 },
        };

        for (size_t i = 0; i < sizeof kSweep / sizeof *kSweep; i++) {
            char label[96];
            int  pics;

            snprintf(label, sizeof label, "Annex-B, %s", kSweep[i].label);
            pics = attempt_decode(query, create, del, decode, flush, getPic,
                                  mapDirect, reset, label, FRAMING_ANNEXB,
                                  kSweep[i].res, kSweep[i].depth, auCount,
                                  queue, /*controls=*/(i == 0), dumpFrame);

            if (pics) {
                annexbPics = pics;
                LOG("\n  *** %s PRODUCED A PICTURE - stopping the sweep ***\n",
                    kSweep[i].label);
                break;
            }
        }
    }
    (void)tryAvcc; (void)avccPics;

    /* -- arbitration LAST, and only if its import actually resolved -------- *
     * Everything above is already in the log and already flushed, so if
     * Initialize still blocks it costs a timeout and nothing else. That is
     * the whole point of moving it here.
     *
     * If it does come up, one more decode runs on a fresh decoder. That is
     * the real test of "the submit is refused because this client is not
     * arbitrated" - the hypothesis findings.md calls the prime suspect for
     * errno 5200, which has never been testable because Initialize hung. */
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
    LOG("  arbitration import slot     : %s\n",
        arbBound ? "BOUND - the modules loaded first supplied it"
                 : "still unbound - Initialize would hang, not called");
    LOG("  arbitration                 : %s\n",
        !useArb ? "skipped" : arbUp ? "initialised and enabled" : "NOT UP");
    LOG("  pictures, Annex-B framing   : %d\n", annexbPics);
    if (annexbPics == 0 && tryAvcc)
        LOG("  pictures, AVCC framing      : %d\n", avccPics);
    else
        LOG("  pictures, AVCC framing      : not attempted\n");
    LOG("  pictures, arbitrated retry  : %s\n",
        arbUp ? (arbPics ? "SEE ABOVE" : "0") : "not attempted");

    if (annexbPics || avccPics || arbPics) {
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
