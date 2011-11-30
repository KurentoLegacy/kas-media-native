#!/bin/bash

# ensure ANDROID_NDK_HOME is set
if [ "" == "$ANDROID_NDK_HOME" ]; then
  echo "Please set ANDROID_NDK_HOME to your Android Native Development Kit path.";
  exit -1;
fi

if [ "" == "$MY_FFMPEG_INSTALL" ]; then
  echo "Please set MY_FFMPEG_INSTALL to the location where ffmpeg libraries should be installed.";
  exit -1;
fi

if [ "" == "$MY_AMR_SOURCE" ]; then
  echo "Please set MY_AMR_SOURCE to the location where AMR source code is located.";
  exit -1;
fi

if [[ "$USE_X264_TREE" != "" && "" == "$MY_X264_INSTALL" ]]; then
  echo "Please set MY_X264_INSTALL to the location where x264 should be installed.";
  exit -1;
fi

MARK_FILE=config.mark

#Check if must run
if [ -f $MARK_FILE ]
then
  OLD_USE_X264_TREE=`grep USE_X264_TREE $MARK_FILE | sed -e "s/USE\_X264\_TREE=//"`
  OLD_ANDROID_NDK_HOME=`grep ANDROID_NDK_HOME $MARK_FILE | sed -e "s/ANDROID\_NDK\_HOME=//"`
  echo "OLD_USE_X264_TREE=$OLD_USE_X264_TREE"
  echo "OLD_ANDROID_NDK_HOME=$OLD_ANDROID_NDK_HOME"
else
  OLD_USE_X264_TREE=""
  OLD_ANDROID_NDK_HOME=""
fi

#if [[ "$OLD_USE_X264_TREE" = "$USE_X264_TREE" && "$OLD_ANDROID_NDK_HOME" = "$ANDROID_NDK_HOME" ]]
if [ -f config.mak ];
then
  echo "No need to run config again, exiting..."
  exit 0;
fi

echo "ANDROID_NDK_HOME=$ANDROID_NDK_HOME" > $MARK_FILE
echo "USE_X264_TREE=$USE_X264_TREE" >> $MARK_FILE

# For Android NDK r5 and 6 (at least), 4.4.3 is used
export armelf=armelf_linux_eabi.x;
export abi=arm-linux-androideabi;
export gccvers=4.4.3
echo "NDK=$(cat $ANDROID_NDK_HOME/RELEASE.TXT); $abi-$gccvers ABI";

export PLATFORM=$ANDROID_NDK_HOME/platforms/android-8/arch-arm
export ARM_INC=$PLATFORM/usr/include
export ARM_LIB=$PLATFORM/usr/lib
export ARM_TOOL=$ANDROID_NDK_HOME/toolchains/$abi-$gccvers/prebuilt/linux-x86
export ARM_LIBO=$ARM_TOOL/lib/gcc/$abi/$gccvers

export MY_CFLAGS="-I$ARM_INC -DANDROID -fpic -mthumb-interwork -ffunction-sections \
		-funwind-tables -fstack-protector -fno-short-enums -D__ARM_ARCH_7A__ \
		-Wno-psabi -march=armv7-a -msoft-float -mthumb -Os -O -fomit-frame-pointer \
		-fno-strict-aliasing -finline-limit=64 -Wa,--noexecstack -MMD -MP "
export MY_LDFLAGS="-L$ARM_LIBO -nostdlib -Bdynamic  -Wl,--no-undefined -Wl,-z,noexecstack  \
		-Wl,-z,nocopyreloc -Wl,-soname,/system/lib/libz.so \
		-Wl,-rpath-link=$PLATFORM/usr/lib,-dynamic-linker=/system/bin/linker \
		-L$ARM_LIB  -lc -lm -ldl -Wl,--library-path=$PLATFORM/usr/lib/ \
		-Xlinker $ARM_LIB/crtbegin_dynamic.o -Xlinker $ARM_LIB/crtend_android.o "

#export USE_X264_TREE=x264-0.106.1741
if [ "" == "$USE_X264_TREE" ]; then
  echo "configure a LGPL ffmpeg, without H264 encoding support"
  export X264_LIB_INC=;
  export X264_LIB_LIB=;
  export X264_C_EXTRA=;
  export X264_LD_EXTRA=;
  export X264_L=;
  export X264_CONFIGURE_OPTS='--disable-gpl --disable-libx264';
else
  echo "configure a GPL ffmpeg, with H264 encoding support at $USE_X264_TREE"
  export X264_SRC=$USE_X264_TREE;
  export X264_LIB_INC=$MY_X264_INSTALL/include;
  export X264_LIB_LIB=$MY_X264_INSTALL/lib;
  export X264_C_EXTRA="-I$X264_LIB_INC";
  export X264_LD_EXTRA="-L$X264_LIB_LIB -rpath-link=$X264_LIB_LIB";
  export X264_L=-lx264;
  cd $X264_SRC;
  echo "configure x264";
  ./config-x264.sh || exit -1;
  cd ..;
  export X264_CONFIGURE_OPTS='--enable-gpl --enable-libx264 --enable-encoder=libx264';
fi

AMR_LIB_INC=$MY_AMR_INSTALL/include
AMR_LIB_LIB=$MY_AMR_INSTALL/lib
cd $MY_AMR_SOURCE
echo "configure OpenCore AMR library"
./config-amr.sh || exit -1
cd ..

./configure --target-os=linux \
	--arch=arm --prefix=${MY_FFMPEG_INSTALL} \
	--enable-cross-compile \
	--cc=$ARM_TOOL/bin/$abi-gcc \
	--cross-prefix=$ARM_TOOL/bin/$abi- \
	--nm=$ARM_TOOL/bin/$abi-nm \
	--enable-static \
	--disable-shared \
	--enable-armv5te --enable-armv6 --enable-armv6t2 --enable-armvfp \
	--disable-asm --disable-yasm --enable-neon --enable-pic \
	--disable-amd3dnow --disable-amd3dnowext --disable-mmx --disable-mmx2 --disable-sse --disable-ssse3 \
	--enable-version3 \
	--disable-nonfree \
	--disable-stripping \
	--disable-doc \
	--disable-ffplay \
	--disable-ffmpeg \
	--disable-ffprobe \
	--disable-ffserver \
	--disable-avdevice \
	--disable-avfilter \
	--disable-devices \
	--disable-encoders \
	--enable-encoder=h263p --enable-encoder=mpeg4 \
	--enable-encoder=libopencore_amrnb --enable-encoder=mp2 --enable-encoder=aac \
	--extra-cflags="-I$AMR_LIB_INC $X264_C_EXTRA " \
	--extra-cflags="$MY_CFLAGS" \
	--extra-ldflags="$MY_LDFLAGS $X264_LD_EXTRA -L$AMR_LIB_LIB -Wl,-T,$ARM_TOOL/$abi/lib/ldscripts/$armelf \
			$ARM_TOOL/lib/gcc/$abi/$gccvers/crtbegin.o $ARM_LIBO/crtend.o -L$ARM_LIBO " \
	--extra-libs="$X264_L -lgcc -lopencore-amrnb " $X264_CONFIGURE_OPTS --enable-libopencore-amrnb \


#	--disable-everything \

#	--disable-muxers --enable-muxer=rtp \
#	--disable-protocols --enable-protocol=rtp \

#	--disable-decoders --enable-decoder=rawvideo \
#	--enable-decoder=h263 --enable-decoder=mpeg4 --enable-decoder=h264 \
#	--enable-decoder=libopencore_amrnb --enable-decoder=mp2 --enable-decoder=aac \
#		error: undefined reference to `ff_vorbis_channel_layouts


#	--disable-filters
#		error: x264_install/include not found



