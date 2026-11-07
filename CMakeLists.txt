# Codec 2 as an ESP-IDF component (and, outside ESP-IDF, as a plain static library).
#
# The library is src/*.c but dump.c, which writes debug dumps to files and is only built with -DDUMP. The C++ files
# in src/ (ButterworthFilter, SampleFilter, FastAudioFIFO) are helpers of the Arduino example, not part of Codec 2,
# and stay out. Arduino and PlatformIO ignore this file and build src/ as they always did (library.properties).

set(CODEC2_SRCS
    "src/codebook.c"
    "src/codebookd.c"
    "src/codebookdt.c"
    "src/codebookge.c"
    "src/codebookjnd.c"
    "src/codebookjvm.c"
    "src/codebooklspmelvq.c"
    "src/codebookmel.c"
    "src/codebookres.c"
    "src/codebookvq.c"
    "src/codebookvqanssi.c"
    "src/codec2.c"
    "src/fifo.c"
    "src/interp.c"
    "src/kiss_fft.c"
    "src/lpc.c"
    "src/lsp.c"
    "src/nlp.c"
    "src/pack.c"
    "src/phase.c"
    "src/phaseexp.c"
    "src/postfilter.c"
    "src/quantise.c"
    "src/sine.c"
)

# include/ holds the public headers only (codec2.h, codec2_fifo.h); src/ has names such as os.h, defines.h and comp.h
# that must not reach the components that use this one.
if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${CODEC2_SRCS}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
    )
    set(CODEC2_TARGET ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    project(codec2 C)
    add_library(codec2 STATIC ${CODEC2_SRCS})
    target_include_directories(codec2 PUBLIC "include" PRIVATE "src")
    target_link_libraries(codec2 PRIVATE m)
    set(CODEC2_TARGET codec2)
endif()

# Upstream's code, built as it is: its warnings are not fixed here, so that the tree stays diffable against upstream,
# and must not stop a project that builds with -Werror. -O2 whatever the project's optimisation level: it is a
# real-time codec, and compiler flags change its float results (README.md, "Compatibility").
target_compile_options(${CODEC2_TARGET} PRIVATE -w -O2)
