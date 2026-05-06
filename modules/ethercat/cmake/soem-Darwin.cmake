# Darwin (macOS) platform file for SOEM — injected via FetchContent patch.
#
# Uses a custom osal (clock_nanosleep is unavailable on macOS) and the macOS
# oshw contrib (pcap/BPF for raw Ethernet frame access).
#
# Requires libpcap: brew install libpcap
# NOTE: macOS EtherCAT is for development/testing only. Deploy on Linux.

find_library(PCAP_LIBRARY pcap REQUIRED)

target_sources(soem PRIVATE
    # Custom osal: same as Linux but uses nanosleep instead of clock_nanosleep.
    osal/macos/osal.c
    # osal_defs.h for macOS lives in osal/macos/ (patched by EtherMouse cmake).
    contrib/oshw/macosx/oshw.c
    contrib/oshw/macosx/oshw.h
    contrib/oshw/macosx/nicdrv.c
    contrib/oshw/macosx/nicdrv.h
)

target_include_directories(soem PUBLIC
    $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/osal/macos>
    $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/contrib/oshw/macosx>
    $<INSTALL_INTERFACE:include/soem>
)

target_link_libraries(soem PUBLIC pthread ${PCAP_LIBRARY})
