/*
 * libhakios.media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of Darwin userland 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/hakios-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/hakios-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/hakios-torch */
static bool camera;                     /* /dev/hakios-camera is there */
extern int hakios_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/hakios-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)hakios_camera_device_create);
			say("hakios: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("hakios: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("hakios: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/hakios-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	if (!said) {
		said = true;
		say("hakios: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
		    "and the camera runs without it\n");
	}
	return true;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("hakios: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("hakios: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("hakios: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("hakios: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that Darwin never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
 */
static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;
	int status;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("hakios: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	status = register_encoder('hvc1', info, create);
	CFRelease(info);
	say("hakios: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
}

/*
 * Safari's green video (PURPLE-umi.md). The frames VideoToolbox's software H.264 decoder in here
 * (H264SW.videocodec) writes into its surfaces are nearly all zero bytes (the kernel's "MiniIOSurface:
 * 420v ... written by mediaserverd" lines). The few bytes that are not grow from frame to frame and the
 * zeros lie in every row: pictures predicted from a reference that is all zeroes -- one that was never
 * decoded, as for a reference frame the decoder was not given (a new surface is all zeroes). There are
 * no "bitstream parsing error" lines from the decoder. So what is the decoder given, sample by sample:
 * the decode time (a step bigger than the one before: samples missing before the decoder), the decode
 * flags (1 asynchronous, 2 not to be shown), the header byte of each NAL unit (0x65 a key frame, 0x41 a
 * P frame others refer to, 0x01 one nobody refers to), whether the units' lengths add up to the sample,
 * its zero bytes, whether its bytes are those of an earlier sample (the WebKit GPU process's samples come
 * through shared memory: stale bytes would repeat), and what the decoder returned. The first 48 samples,
 * every 64th, and the first 32 that look wrong. Measures only.
 *
 * The decoder's VTVideoDecoderClass (20F66, unslid; __DATA_DIRTY, writable and each process's own):
 * version 3, startSession, decodeFrame, copySupportedPropertyDictionary, ...
 */
#define H264SW_START_SESSION_SLOT       0x213c899c8UL
#define H264SW_START_SESSION            0x1f0350bccUL
#define H264SW_DECODE_FRAME_SLOT        0x213c899d0UL
#define H264SW_DECODE_FRAME             0x1f0350c5cUL

typedef OSStatus (*decode_frame_fn)(void *decoder, void *frame, CMSampleBufferRef sample, uint32_t flags, uint32_t *info);
static decode_frame_fn original_decode_frame;

/*
 * The cause (PURPLE-umi.md, "root cause"): H264SW runs the decoding process for gaps in frame_num
 * (8.2.5.2) also for a picture whose frame_num equals that of the reference picture before it, which
 * the standard does not. Safari's stream numbers its pictures that way (I 0 | P8 1, B4 2, B2 3, b1 3,
 * b3 3, B6 4, ...) and its SPS allows gaps (gaps_in_frame_num_value_allowed_flag): at b1 the decoder
 * takes the 4095 frame numbers from 4 round to 2 as missing, fills the DPB with as many frames that
 * never were, and from there on every picture is predicted from nothing -- nearly all zero bytes,
 * green. A Mac's software decoder does the same; the hardware decoders (an iPhone never uses H264SW)
 * and ffmpeg decode the stream. With the flag clear H264SW decodes it as they do, so that is what it
 * is given: the SPS in the avcC of the format description it is started with (it reads no other), and
 * any SPS in a sample (it takes that over the avcC's). A stream that really skips frame numbers is then
 * decoded as one with frames lost.
 */
typedef OSStatus (*start_session_fn)(void *decoder, void *session, CMVideoFormatDescriptionRef format);
static start_session_fn original_start_session;

struct rbsp {
	const uint8_t *p;
	size_t n, byte, at;             /* the next bit: p[byte], bit `bit`; the last one read: p[at], bit `at_bit` */
	int bit, at_bit, zeros;
	bool bad;
};

static unsigned
rbsp_u(struct rbsp *r, int bits)
{
	unsigned v = 0;

	while (bits-- > 0) {
		if (r->bit == 8) {      /* the next byte, past an emulation prevention byte (00 00 03) */
			r->zeros = r->p[r->byte] == 0 ? r->zeros + 1 : 0;
			r->byte++;
			r->bit = 0;
			if (r->zeros >= 2 && r->byte < r->n && r->p[r->byte] == 3) {
				r->byte++;
				r->zeros = 0;
			}
		}
		if (r->byte >= r->n) {
			r->bad = true;
			return 0;
		}
		r->at = r->byte;
		r->at_bit = r->bit;
		v = v << 1 | ((r->p[r->byte] >> (7 - r->bit++)) & 1);
	}
	return v;
}

static unsigned
rbsp_ue(struct rbsp *r)
{
	int zeros = 0;

	while (!r->bad && zeros < 32 && rbsp_u(r, 1) == 0) {
		zeros++;
	}
	return zeros < 32 ? (1u << zeros) - 1 + rbsp_u(r, zeros) : (r->bad = true, 0);
}

/* In an SPS NAL unit (its header byte first), where its gaps_in_frame_num_value_allowed_flag is,
 * when it is set: p[*byte] & mask. */
static bool
sps_gaps_allowed(const uint8_t *p, size_t n, size_t *byte, uint8_t *mask)
{
	struct rbsp r = { .p = p, .n = n };
	unsigned profile;

	rbsp_u(&r, 8);                  /* the NAL unit header */
	profile = rbsp_u(&r, 8);
	rbsp_u(&r, 16);
	rbsp_ue(&r);
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
	    profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
	    profile == 135) {
		const unsigned chroma = rbsp_ue(&r);

		if (chroma == 3) {
			rbsp_u(&r, 1);
		}
		rbsp_ue(&r);
		rbsp_ue(&r);
		rbsp_u(&r, 1);
		if (rbsp_u(&r, 1)) {    /* scaling matrices */
			for (int i = 0; i < (chroma == 3 ? 12 : 8) && !r.bad; i++) {
				if (rbsp_u(&r, 1) == 0) {
					continue;
				}
				for (int j = 0, last = 8, next = 8; j < (i < 6 ? 16 : 64) && next != 0 && !r.bad; j++) {
					const unsigned v = rbsp_ue(&r);

					next = (last + (v & 1 ? (int)(v + 1) / 2 : -(int)(v / 2)) + 256) % 256;
					last = next != 0 ? next : last;
				}
			}
		}
	}
	rbsp_ue(&r);                    /* log2_max_frame_num_minus4 */
	switch (rbsp_ue(&r)) {          /* pic_order_cnt_type */
	case 0:
		rbsp_ue(&r);
		break;
	case 1:
		rbsp_u(&r, 1);
		rbsp_ue(&r);
		rbsp_ue(&r);
		for (unsigned i = 0, cycle = rbsp_ue(&r); i < cycle && !r.bad; i++) {
			rbsp_ue(&r);
		}
		break;
	}
	rbsp_ue(&r);                    /* max_num_ref_frames */
	if (rbsp_u(&r, 1) == 0 || r.bad) {
		return false;
	}
	*byte = r.at;
	*mask = (uint8_t)(0x80 >> r.at_bit);
	return true;
}

/* the format description with the flag clear in its avcC's SPS; NULL if no SPS there has it set */
static CMVideoFormatDescriptionRef
gapless_format(CMVideoFormatDescriptionRef format)
{
	CFDictionaryRef atoms = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
	CFDataRef avcc = atoms != NULL && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, CFSTR("avcC")) : NULL;
	CMVideoFormatDescriptionRef gapless = NULL;

	if (avcc == NULL || CFGetTypeID(avcc) != CFDataGetTypeID() || CFDataGetLength(avcc) < 6) {
		return NULL;
	}

	CFMutableDataRef data = CFDataCreateMutableCopy(NULL, 0, avcc);
	uint8_t *p = CFDataGetMutableBytePtr(data);
	const size_t n = (size_t)CFDataGetLength(data);
	bool cleared = false;

	/* avcC: version, profile, compatibility, level, length size, number of SPS, then each: 16-bit size, SPS */
	for (size_t i = 0, at = 6, size; i < (p[5] & 0x1fu) && at + 2 <= n; i++, at += 2 + size) {
		size_t byte;
		uint8_t mask;

		size = (size_t)p[at] << 8 | p[at + 1];
		if (size > n - at - 2) {
			break;
		}
		if (sps_gaps_allowed(p + at + 2, size, &byte, &mask)) {
			p[at + 2 + byte] &= (uint8_t)~mask;
			cleared = true;
		}
	}
	if (cleared) {
		CFMutableDictionaryRef new_atoms = CFDictionaryCreateMutableCopy(NULL, 0, atoms);
		CFMutableDictionaryRef extensions = CFDictionaryCreateMutableCopy(NULL, 0, CMFormatDescriptionGetExtensions(format));
		const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);

		CFDictionarySetValue(new_atoms, CFSTR("avcC"), data);
		CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, new_atoms);
		CMVideoFormatDescriptionCreate(NULL, CMFormatDescriptionGetMediaSubType(format), size.width, size.height, extensions,
		    &gapless);
		CFRelease(extensions);
		CFRelease(new_atoms);
	}
	CFRelease(data);
	return gapless;
}

static OSStatus
h264_start_session(void *decoder, void *session, CMVideoFormatDescriptionRef format)
{
	CMVideoFormatDescriptionRef gapless = gapless_format(format);
	OSStatus status;

	if (gapless == NULL) {
		return original_start_session(decoder, session, format);
	}
	say("hakios: mediaserverd: H264SW: the SPS in the avcC allows gaps in frame_num; started without\n");
	status = original_start_session(decoder, session, gapless);
	CFRelease(gapless);
	return status;
}

static void
copy_attachment(const void *key, const void *value, void *to)
{
	CFDictionarySetValue(to, key, value);
}

/* the sample with the flag clear in each SPS it carries; NULL if none there has it set */
static CMSampleBufferRef
gapless_sample(CMSampleBufferRef sample)
{
	CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample), block = NULL;
	CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
	const size_t length = data != NULL ? CMBlockBufferGetDataLength(data) : 0;
	uint8_t *bytes = length != 0 ? malloc(length) : NULL;
	CMSampleBufferRef gapless = NULL;
	bool cleared = false;
	int header = 4;

	if (bytes == NULL || CMBlockBufferCopyDataBytes(data, 0, length, bytes) != kCMBlockBufferNoErr) {
		free(bytes);
		return NULL;
	}
	CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, NULL, &header);
	for (size_t at = 0, size; at + (size_t)header < length; at += (size_t)header + size) {
		size_t byte;
		uint8_t mask;

		size = 0;
		for (int i = 0; i < header; i++) {
			size = size << 8 | bytes[at + (size_t)i];
		}
		if (size == 0 || size > length - at - (size_t)header) {
			break;
		}
		if ((bytes[at + (size_t)header] & 0x1f) == 7 && sps_gaps_allowed(bytes + at + header, size, &byte, &mask)) {
			bytes[at + (size_t)header + byte] &= (uint8_t)~mask;
			cleared = true;
		}
	}
	if (!cleared || CMBlockBufferCreateWithMemoryBlock(NULL, bytes, length, kCFAllocatorMalloc, NULL, 0, length, 0, &block) != 0) {
		free(bytes);
		return NULL;
	}

	CMItemCount timings = 0, sizes = 0;

	CMSampleBufferGetSampleTimingInfoArray(sample, 0, NULL, &timings);
	CMSampleBufferGetSampleSizeArray(sample, 0, NULL, &sizes);

	CMSampleTimingInfo timing[timings > 0 ? timings : 1];
	size_t size[sizes > 0 ? sizes : 1];

	CMSampleBufferGetSampleTimingInfoArray(sample, timings, timing, NULL);
	CMSampleBufferGetSampleSizeArray(sample, sizes, size, NULL);
	if (CMSampleBufferCreate(NULL, block, true, NULL, NULL, format, CMSampleBufferGetNumSamples(sample), timings, timing, sizes, size,
	    &gapless) == 0) {
		const CMAttachmentMode modes[] = { kCMAttachmentMode_ShouldPropagate, kCMAttachmentMode_ShouldNotPropagate };
		CFArrayRef from = CMSampleBufferGetSampleAttachmentsArray(sample, false);
		CFArrayRef to = from != NULL ? CMSampleBufferGetSampleAttachmentsArray(gapless, true) : NULL;

		for (int i = 0; i < 2; i++) {
			CFDictionaryRef attachments = CMCopyDictionaryOfAttachments(NULL, sample, modes[i]);

			if (attachments != NULL) {
				CMSetAttachments(gapless, attachments, modes[i]);
				CFRelease(attachments);
			}
		}
		for (CFIndex i = 0; to != NULL && i < CFArrayGetCount(from) && i < CFArrayGetCount(to); i++) {
			CFDictionaryApplyFunction(CFArrayGetValueAtIndex(from, i), copy_attachment, (void *)CFArrayGetValueAtIndex(to, i));
		}
	}
	CFRelease(block);
	return gapless;
}

static OSStatus
h264_measured_decode_frame(void *decoder, void *frame, CMSampleBufferRef sample, uint32_t flags, uint32_t *info);

static OSStatus
h264_decode_frame(void *decoder, void *frame, CMSampleBufferRef sample, uint32_t flags, uint32_t *info)
{
	static uint32_t cleared;
	CMSampleBufferRef gapless = gapless_sample(sample);
	OSStatus status;

	if (gapless == NULL) {
		return h264_measured_decode_frame(decoder, frame, sample, flags, info);
	}
	if (__atomic_fetch_add(&cleared, 1, __ATOMIC_RELAXED) < 4) {
		say("hakios: mediaserverd: H264SW: an SPS in a sample allows gaps in frame_num; decoded without\n");
	}
	status = h264_measured_decode_frame(decoder, frame, gapless, flags, info);
	CFRelease(gapless);
	return status;
}

static OSStatus
h264_measured_decode_frame(void *decoder, void *frame, CMSampleBufferRef sample, uint32_t flags, uint32_t *info)
{
	static uint32_t samples, odd, seen_hash[64], seen_sample[64];
	static double last_dts, last_step;
	CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
	const size_t length = data != NULL ? CMBlockBufferGetDataLength(data) : 0;
	uint8_t *bytes = length != 0 ? malloc(length) : NULL;

	if (bytes == NULL || CMBlockBufferCopyDataBytes(data, 0, length, bytes) != kCMBlockBufferNoErr) {
		free(bytes);
		return original_decode_frame(decoder, frame, sample, flags, info);
	}

	const uint32_t n = __atomic_fetch_add(&samples, 1, __ATOMIC_RELAXED);
	int header = 4;
	char units[28] = "", lengths[40] = "lengths add up", same[40] = "";
	size_t used = 0, at = 0, zeros = 0;
	unsigned count = 0;
	uint32_t hash = 2166136261u;
	bool wrong = false;

	CMVideoFormatDescriptionGetH264ParameterSetAtIndex(CMSampleBufferGetFormatDescription(sample), 0, NULL, NULL, NULL, &header);
	while (at + (size_t)header <= length) {
		size_t size = 0;

		for (int i = 0; i < header; i++) {
			size = size << 8 | bytes[at + (size_t)i];
		}
		if (size == 0 || size > length - at - (size_t)header) {
			break;
		}
		if (used + 4 < sizeof(units)) {
			used += (size_t)snprintf(units + used, sizeof(units) - used, " %02x", bytes[at + (size_t)header]);
		}
		count++;
		at += (size_t)header + size;
	}
	if (at != length) {
		snprintf(lengths, sizeof(lengths), "lengths break at byte %zu", at);
		wrong = true;
	}
	for (size_t i = 0; i < length; i++) {
		zeros += bytes[i] == 0;
		hash = (hash ^ bytes[i]) * 16777619u;
	}
	free(bytes);
	for (unsigned k = 0; k < 64; k++) {
		if (seen_sample[k] != 0 && seen_hash[k] == hash) {
			snprintf(same, sizeof(same), ", the bytes of sample %u", seen_sample[k] - 1);
			wrong = true;
		}
	}
	seen_hash[n % 64] = hash;
	seen_sample[n % 64] = n + 1;

	CMTime time = CMSampleBufferGetDecodeTimeStamp(sample);
	if (!CMTIME_IS_VALID(time)) {
		time = CMSampleBufferGetPresentationTimeStamp(sample);
	}
	const double dts = CMTimeGetSeconds(time) * 1000, step = dts - last_dts;
	const bool gap = n != 0 && last_step > 0 && step > last_step * 1.5;

	wrong |= gap;
	last_step = n != 0 ? step : 0;
	last_dts = dts;

	const OSStatus status = original_decode_frame(decoder, frame, sample, flags, info);

	if (n < 48 || n % 64 == 0 || (wrong && __atomic_fetch_add(&odd, 1, __ATOMIC_RELAXED) < 32)) {
		say("hakios: mediaserverd: H.264 sample %u to the decoder: dts %.1f ms (+%.1f%s), flags %#x, %zu bytes, %u NAL units%s, "
		    "%s, %zu zero bytes, hash %08x%s; decoded: %d\n", n, dts, step, gap ? ", samples missing" : "", flags, length,
		    count, units, lengths, zeros, hash, same, (int)status);
	}
	return status;
}

static void
watch_h264_samples(void)
{
	uintptr_t *entry = (uintptr_t *)(H264SW_DECODE_FRAME_SLOT + slide);

	if (*entry != H264SW_DECODE_FRAME + slide) {
		say("hakios: mediaserverd: H264SW's decodeFrame: found %#lx, expected %#lx; left alone\n", *entry - slide,
		    H264SW_DECODE_FRAME);
		return;
	}
	original_decode_frame = (decode_frame_fn)*entry;
	*entry = (uintptr_t)h264_decode_frame;
	say("hakios: mediaserverd: H.264 samples are measured as H264SW's decoder takes them\n");

	entry = (uintptr_t *)(H264SW_START_SESSION_SLOT + slide);
	if (*entry != H264SW_START_SESSION + slide) {
		say("hakios: mediaserverd: H264SW's startSession: found %#lx, expected %#lx; left alone\n", *entry - slide,
		    H264SW_START_SESSION);
		return;
	}
	original_start_session = (start_session_fn)*entry;
	*entry = (uintptr_t)h264_start_session;
	say("hakios: mediaserverd: H264SW is given its SPS without gaps in frame_num\n");
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("HakiosFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("HakiosFlashlightDevice", NULL, 0);
	stream_class = make_class("HakiosFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in Darwin userland 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
};

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("hakios: mediaserverd: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
	}
}

__attribute__((constructor))
static void
hakios_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_h264_samples();
	torch = open("/dev/hakios-torch", O_WRONLY);
	camera = access("/dev/hakios-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("hakios: mediaserverd: the kernel has a flashlight (/dev/hakios-torch)\n");
	}
	if (camera) {
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
		register_hevc_encoder();
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
