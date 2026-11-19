#!/bin/sh
# Builds vtdec_test for the phone (iOS 16.5 SDK) into ./vtdec_test. Copy vtdec_test, pyramid.h264, vt.h264 to the phone.
set -e
cd "$(dirname "$0")"
SDK=/Users/xm666/X/umi-ios/tools/theos/sdks/iPhoneOS16.5.sdk
xcrun clang -target arm64-apple-ios16.5 -isysroot "$SDK" -O1 -Wall -Wno-four-char-constants -o vtdec_test vtdec_test.c \
	-framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework IOSurface
# test clips, 478x360 like the Safari video: x264 (P, one reference B, six non-reference b) and VideoToolbox (the Safari stream's NAL pattern)
[ -f pyramid.h264 ] || ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=478x360:rate=30 -t 2 -pix_fmt yuv420p -c:v libx264 -profile:v high \
	-x264-params bframes=7:b-pyramid=normal:b-adapt=0:keyint=60:min-keyint=60:scenecut=0:aud=1:ref=4 -bsf:v h264_mp4toannexb -f h264 pyramid.h264
[ -f vt.h264 ] || ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=478x360:rate=30 -t 2 -pix_fmt yuv420p -c:v h264_videotoolbox -profile:v high \
	-bf 3 -b:v 1M -g 60 -bsf:v h264_mp4toannexb -f h264 vt.h264
# jm-*.h264: Safari's stream's structure, I | P8 B4 B2 b1 b3 B6 b5 b7 (two levels of reference B), which neither
# encoder above makes. Made with the JM 19 reference encoder (vcgit.hhi.fraunhofer.de/jvet/JM, cmake, lencod) from the
# same testsrc2 frames as raw yuv420p (src478.yuv); plain: max_num_ref_frames 5; ref4: 4 (as VideoToolbox's encoder
# writes); mmco: plus JM's POC-distance reordering and memory management commands:
#   lencod -d cfg/encoder_main.cfg -p InputFile=src478.yuv -p SourceWidth=478 -p SourceHeight=360 -p FramesToBeEncoded=60 \
#     -p ProfileIDC=100 -p LevelIDC=30 -p IntraPeriod=0 -p IDRPeriod=0 -p NumberBFrames=7 -p QPISlice=26 -p QPPSlice=28 \
#     -p QPBSlice=30 -p NumberReferenceFrames=5 -p HierarchicalCoding=3 -p ExplicitHierarchyFormat=b3r0b1r0b0e0b2e0b5r0b4e0b6e0 \
#     -p SymbolMode=1 -p Transform8x8Mode=1 -p SearchMode=3 -p PocMemoryManagement=0 -p ReferenceReorder=0 -p OutputFile=jm-plain.h264
ls -la vtdec_test pyramid.h264 vt.h264 jm-*.h264
