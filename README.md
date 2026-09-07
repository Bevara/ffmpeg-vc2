# ffmpeg-vc2
This filter decodes VC-2 (SMPTE ST 2042) elementary streams, using a reduced version of ffmpeg carrying the Dirac decoder only.

> **This accessor is not verified.** It builds, it loads, and it moves every
> frame of a stream through the chain — but no decode of it has ever been shown
> to be correct, because there is nothing here to check it against. Read
> [Status](#status) before using it for anything.

## Status

VC-2 is the intra-only profile family that came out of Dirac, and the High
Quality profile is what production equipment and ffmpeg's `vc2` encoder emit.
schroedinger — which [libschro](https://github.com/Bevara/libschro) drives, and
which decodes Dirac — predates that profile and refuses its pictures outright,
parse code `0xE8`. ffmpeg's Dirac decoder claims to cover VC-2, which is what
this accessor is built on.

What has actually been measured:

- ffmpeg decodes a **Dirac** stream correctly (luma min 14, max 237 on the
  first frame of the reference `.drc`).
- ffmpeg **cannot decode the output of its own `vc2` encoder**. The frame comes
  back a uniform 128 across the whole luma plane — the value you get when the
  wavelet transform has written nothing and `put_signed_rect_clamped` adds its
  offset. Reproduced with ffmpeg 8.x and with the 6.1.2 this accessor is built
  from.

So ffmpeg can serve neither as the decoder under test nor as the reference, and
this accessor decodes that same stream to the same grey. Whether the fault is
in ffmpeg's `vc2` encoder or in its High Quality decode path cannot be settled
without a third implementation.

Two things would settle it:

- a **VC-2 HQ conformance stream** from somewhere other than ffmpeg;
- or the BBC's [vc2-reference](https://github.com/bbc/vc2-reference) encoder.
  Contrary to what is often assumed, it already embeds its boost subset in
  `src/boost`; what it needs is the compiled boost libraries its `configure.ac`
  asks for (`program_options`, `thread`, `system`), plus one line relaxing an
  architecture check that refuses anything but x86_64.

## What the filter does

Framing is done in the filter rather than by a parser. A VC-2 stream is a chain
of 13-byte parse info headers — `BBCD`, a parse code, then the offsets to the
next and previous one — so walking it needs no bit reading, and one sequence
header to the next is exactly one frame.

The sequence header does need bit reading, for the picture size: everything
downstream is resolved from the size announced at configure time, and a decoder
cannot wait for its first frame to say what it produces. Dirac codes integers
as *interleaved* exponential Golomb — a continuation bit, then a data bit,
repeated. Reading all the zeroes first and the data after, the non-interleaved
form, parses the same bytes into different numbers and says nothing: a 320x180
sequence header comes out as 7x393.

Output is 4:2:0 whatever the stream codes. VC-2 is commonly 4:2:2 or 4:4:4, and
announcing the real format would mean not knowing it until the first frame,
which is after the graph has been resolved; swscale converts, so the
announcement is true from the start.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute ffmpeg-vc2_1.wasm with your universal tags.

## Rebuilding the ffmpeg libraries

```bash
emconfigure $FFMPEG_SRC/configure --target-os=none --arch=x86_32 \
    --enable-cross-compile --disable-x86asm --disable-inline-asm \
    --disable-stripping --disable-programs --disable-doc \
    --disable-runtime-cpudetect --disable-autodetect --disable-pthreads \
    --pkg-config-flags="--static" --nm="$EMSDK/upstream/bin/llvm-nm" \
    --ar=emar --ranlib=emranlib --cc=emcc --cxx=em++ --objcc=emcc --dep-cc=emcc \
    --enable-pic --disable-everything --enable-decoder=dirac
emmake make
```

ffmpeg's configure needs one fix first. `diracdsp`'s dispatch table points at
`ff_put_dirac_pixels*` and `ff_avg_dirac_pixels*`, which live in `qpeldsp.c`,
but `dirac_decoder_select` does not list `qpeldsp`. A full ffmpeg build has it
anyway, so nothing shows; with `--disable-everything` the module comes out with
eighteen undefined symbols and refuses to load:

```
dirac_decoder_select="dirac_parse dwt golomb qpeldsp videodsp mpegvideoenc"
```

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
